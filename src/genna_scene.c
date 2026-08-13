/* genna_scene.c — 3D spatial layer. See genna_scene.h for the design and for
 * what each quantization mode actually buys. */
#include "../include/genna_scene.h"
#include "../include/genna_persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ====================================================================== */
/* OBJ loading                                                            */
/* ====================================================================== */

int gn_mesh_load_obj(const char *path, gn_mesh *out) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t cap = 1u << 16, n = 0;
    float *v = malloc(cap * 3 * sizeof(float));
    if (!v) { fclose(f); return -1; }

    char line[512];
    while (fgets(line, sizeof line, f)) {
        /* only 'v ' -- 'vn'/'vt' are normals/uvs and are not positions */
        if (line[0] != 'v' || (line[1] != ' ' && line[1] != '\t')) continue;
        double x, y, z;
        if (sscanf(line + 1, "%lf %lf %lf", &x, &y, &z) != 3) continue;
        if (n == cap) {
            cap *= 2;
            float *nv = realloc(v, cap * 3 * sizeof(float));
            if (!nv) { free(v); fclose(f); return -1; }
            v = nv;
        }
        v[n * 3 + 0] = (float)x;
        v[n * 3 + 1] = (float)y;
        v[n * 3 + 2] = (float)z;
        n++;
    }
    fclose(f);
    if (n == 0) { free(v); return -1; }
    out->xyz = v;
    out->n = (uint32_t)n;
    return 0;
}

void gn_mesh_free(gn_mesh *m) {
    if (!m) return;
    free(m->xyz);
    m->xyz = NULL; m->n = 0;
}

void gn_mesh_bounds(const gn_mesh *m, float lo[3], float hi[3]) {
    for (int k = 0; k < 3; k++) { lo[k] = 1e30f; hi[k] = -1e30f; }
    for (uint32_t i = 0; i < m->n; i++)
        for (int k = 0; k < 3; k++) {
            float c = m->xyz[i * 3 + k];
            if (c < lo[k]) lo[k] = c;
            if (c > hi[k]) hi[k] = c;
        }
}

/* ====================================================================== */
/* quantization                                                           */
/* ====================================================================== */

static inline uint32_t f2b(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float    b2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void put_i32(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}
static int32_t get_i32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* The grid point a coordinate snaps to. Done in double so the rounding is
 * the same on every platform, then narrowed once. */
static inline float grid_point(int32_t q, float step) {
    return (float)((double)q * (double)step);
}

int gn_quant_encode(const gn_mesh *m, float step, gn_quant_mode mode,
                    gn_quantized *out) {
    memset(out, 0, sizeof *out);
    if (!m || !m->n || !(step > 0.0f)) return -1;

    size_t ncoord = (size_t)m->n * 3;
    out->grid_len = ncoord * 4;
    out->grid = malloc(out->grid_len);
    if (!out->grid) return -1;

    if (mode == GN_QUANT_EXACT)      out->resid_len = ncoord * 4;
    else if (mode == GN_QUANT_BYTE)  out->resid_len = ncoord;
    else                             out->resid_len = 0;

    if (out->resid_len) {
        out->resid = malloc(out->resid_len);
        if (!out->resid) { free(out->grid); out->grid = NULL; return -1; }
    }

    for (size_t i = 0; i < ncoord; i++) {
        float x = m->xyz[i];
        int32_t q = (int32_t)llround((double)x / (double)step);
        put_i32(out->grid + i * 4, q);

        if (mode == GN_QUANT_EXACT) {
            /* XOR against the reconstructed grid point. Nearby values share
             * their sign/exponent/high mantissa bits, so this residual is
             * mostly leading zeros -- but it is still a full 4 bytes and it
             * still changes whenever x does. That is the cost of exactness. */
            uint32_t r = f2b(x) ^ f2b(grid_point(q, step));
            put_i32(out->resid + i * 4, (int32_t)r);
        } else if (mode == GN_QUANT_BYTE) {
            /* 255 levels across the cell => spacing step/255, so the worst
             * error is step/510. Not step/512: measured at 9.83e-07 against
             * a step of 5e-04, which is step/509. */
            double frac = ((double)x / (double)step) - (double)q;   /* [-0.5,0.5] */
            int lvl = (int)llround((frac + 0.5) * 255.0);
            if (lvl < 0) lvl = 0;
            if (lvl > 255) lvl = 255;
            out->resid[i] = (uint8_t)lvl;
        }
    }
    out->step = step;
    out->mode = mode;
    out->n = m->n;
    return 0;
}

int gn_quant_decode(const gn_quantized *q, gn_mesh *out) {
    memset(out, 0, sizeof *out);
    if (!q || !q->n) return -1;
    size_t ncoord = (size_t)q->n * 3;
    out->xyz = malloc(ncoord * sizeof(float));
    if (!out->xyz) return -1;
    out->n = q->n;

    for (size_t i = 0; i < ncoord; i++) {
        int32_t g = get_i32(q->grid + i * 4);
        float base = grid_point(g, q->step);
        if (q->mode == GN_QUANT_EXACT) {
            uint32_t r = (uint32_t)get_i32(q->resid + i * 4);
            out->xyz[i] = b2f(f2b(base) ^ r);
        } else if (q->mode == GN_QUANT_BYTE) {
            double frac = ((double)q->resid[i] / 255.0) - 0.5;
            out->xyz[i] = (float)(((double)g + frac) * (double)q->step);
        } else {
            out->xyz[i] = base;
        }
    }
    return 0;
}

void gn_quant_free(gn_quantized *q) {
    if (!q) return;
    free(q->grid); free(q->resid);
    q->grid = q->resid = NULL; q->grid_len = q->resid_len = 0;
}

double gn_mesh_max_error(const gn_mesh *a, const gn_mesh *b, uint32_t *worst) {
    if (!a || !b || a->n != b->n) return -1.0;
    double mx = 0.0;
    uint32_t at = 0;
    for (size_t i = 0; i < (size_t)a->n * 3; i++) {
        double d = fabs((double)a->xyz[i] - (double)b->xyz[i]);
        if (d > mx) { mx = d; at = (uint32_t)(i / 3); }
    }
    if (worst) *worst = at;
    return mx;
}

/* ====================================================================== */
/* scene                                                                  */
/* ====================================================================== */

struct gn_scene {
    gn_engine *e;
    gn_object *inst;            /* packed instance records */
    uint32_t   n_inst;
    uint32_t   n_asset;
};

void gn_mat_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
void gn_mat_translation(float m[16], float x, float y, float z) {
    gn_mat_identity(m);
    m[3] = x; m[7] = y; m[11] = z;
}

gn_scene *gn_scene_new(gn_engine *e) {
    if (!e) return NULL;
    gn_scene *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->e = e;
    /* The instance table is itself a Genna object, so it gets versioning,
     * rollback and persistence for free -- moving a building is an edit in
     * the same history as everything else. */
    s->inst = gn_create(e, "__instances", NULL, 0);
    if (!s->inst) { free(s); return NULL; }
    return s;
}

void gn_scene_free(gn_scene *s) { free(s); }

static void pack_instance(uint8_t *dst, uint32_t asset, const float m[16]) {
    put_i32(dst, (int32_t)asset);
    for (int k = 0; k < 16; k++) put_i32(dst + 4 + k * 4, (int32_t)f2b(m[k]));
}

uint32_t gn_scene_add_asset(gn_scene *s, const char *name,
                            const gn_quantized *q) {
    if (!s || !q) return (uint32_t)-1;
    char nm[64];

    /* Grid and residual go into SEPARATE objects. If they shared one, a
     * changed residual would land in the same chunk as unchanged grid data
     * and break its hash -- destroying exactly the sharing this design is
     * for. Keeping them apart is the whole trick. */
    snprintf(nm, sizeof nm, "%.40s.grid", name);
    if (!gn_create(s->e, nm, q->grid, q->grid_len)) return (uint32_t)-1;

    if (q->resid_len) {
        snprintf(nm, sizeof nm, "%.40s.resid", name);
        if (!gn_create(s->e, nm, q->resid, q->resid_len)) return (uint32_t)-1;
    }
    return s->n_asset++;
}

uint32_t gn_scene_add_instance(gn_scene *s, uint32_t asset, const float m[16]) {
    if (!s) return (uint32_t)-1;
    uint8_t rec[GN_INSTANCE_BYTES];
    pack_instance(rec, asset, m);
    uint64_t at = (uint64_t)s->n_inst * GN_INSTANCE_BYTES;
    if (gn_update(s->e, s->inst, at, 0, rec, sizeof rec) != 0)
        return (uint32_t)-1;
    return s->n_inst++;
}

int gn_scene_set_transform(gn_scene *s, uint32_t instance, const float m[16]) {
    if (!s || instance >= s->n_inst) return -1;
    uint8_t rec[GN_INSTANCE_BYTES];
    /* keep the asset id, replace the matrix */
    uint8_t cur[GN_INSTANCE_BYTES];
    uint64_t at = (uint64_t)instance * GN_INSTANCE_BYTES;
    if (gn_read(s->e, s->inst, at, GN_INSTANCE_BYTES, cur) != GN_INSTANCE_BYTES)
        return -1;
    pack_instance(rec, (uint32_t)get_i32(cur), m);
    return gn_update(s->e, s->inst, at, GN_INSTANCE_BYTES, rec, sizeof rec);
}

int gn_scene_get_transform(const gn_scene *s, uint32_t instance, float m[16]) {
    if (!s || instance >= s->n_inst) return -1;
    uint8_t cur[GN_INSTANCE_BYTES];
    uint64_t at = (uint64_t)instance * GN_INSTANCE_BYTES;
    if (gn_read(s->e, s->inst, at, GN_INSTANCE_BYTES, cur) != GN_INSTANCE_BYTES)
        return -1;
    for (int k = 0; k < 16; k++) m[k] = b2f((uint32_t)get_i32(cur + 4 + k * 4));
    return 0;
}

int gn_scene_translate(gn_scene *s, uint32_t instance,
                       float dx, float dy, float dz) {
    float m[16];
    if (gn_scene_get_transform(s, instance, m) != 0) return -1;
    m[3] += dx; m[7] += dy; m[11] += dz;
    return gn_scene_set_transform(s, instance, m);
}

uint32_t   gn_scene_instances(const gn_scene *s) { return s ? s->n_inst : 0; }
uint32_t   gn_scene_assets(const gn_scene *s)    { return s ? s->n_asset : 0; }
gn_object *gn_scene_instance_object(const gn_scene *s) { return s ? s->inst : NULL; }
