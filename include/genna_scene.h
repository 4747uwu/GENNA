/* genna_scene.h — 3D spatial layer over the Genna engine.
 *
 * Two independent ideas, kept separate because they succeed and fail for
 * different reasons and must be measured separately:
 *
 *  1. INSTANCE TABLE (the transform idea, correctly homed).
 *     Geometry is stored once as an asset; the scene is a list of
 *     (asset, 4x4 matrix) records. Moving an object rewrites 68 bytes in
 *     that list, not a single vertex.
 *
 *     Note this does NOT hang transforms off treap nodes. A treap node is a
 *     position in a sequence, not an entity, and path copying gives it a new
 *     identity on every edit -- there is no stable node that means "this
 *     building". The instance table is the stable thing, and it gets the
 *     same win.
 *
 *  2. QUANTIZATION (the float-noise idea).
 *     Vertex positions are split into a quantized grid stream and a residual
 *     stream. Sub-quantum jitter leaves the quantized stream byte-identical,
 *     so it dedups against previous versions; the residual stream carries
 *     whatever precision is being kept.
 *
 *     The honest catch: in EXACT mode the residual stream IS the noise. It
 *     changes whenever the input does and dedups nothing, so exact mode can
 *     only ever recover the quantized half. Lossy modes give up bounded
 *     precision to make the residual dedup too. gn_quant_mode says which
 *     you are buying; the tests measure what each is actually worth.
 */
#ifndef GENNA_SCENE_H
#define GENNA_SCENE_H

#include "genna.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- meshes ---------------------------------------------------------- */

typedef struct {
    float   *xyz;        /* 3 floats per vertex, interleaved */
    uint32_t n;          /* vertex count                     */
} gn_mesh;

/* Load vertex positions from a Wavefront OBJ ('v x y z' lines).
 * Returns 0 on success. Only positions are read: this layer is about
 * spatial entropy, and normals/uvs would just be more of the same. */
int  gn_mesh_load_obj(const char *path, gn_mesh *out);
void gn_mesh_free(gn_mesh *m);

/* Bounding box, for choosing a sane quantization step. */
void gn_mesh_bounds(const gn_mesh *m, float lo[3], float hi[3]);

/* ---- quantization codec ---------------------------------------------- */

typedef enum {
    /* Byte-exact. Residual is the XOR of the float bits against the
     * reconstructed grid point, so decode returns the input exactly. */
    GN_QUANT_EXACT = 0,
    /* Lossy, bounded: |error| <= step/2. No residual stream at all. */
    GN_QUANT_GRID = 1,
    /* Lossy, bounded: |error| <= step/500. One residual byte per coord.
     * (255 levels/cell => spacing step/255, worst error step/510, plus float32
     * rounding of the reconstruction. Stated as step/512 originally; the codec
     * cannot meet that, which the fidelity test caught.) */
    GN_QUANT_BYTE = 2,
} gn_quant_mode;

typedef struct {
    uint8_t  *grid;      /* quantized coordinates                        */
    size_t    grid_len;
    uint8_t  *resid;     /* residual stream (NULL for GN_QUANT_GRID)     */
    size_t    resid_len;
    float     step;
    gn_quant_mode mode;
    uint32_t  n;
} gn_quantized;

/* Split a mesh into grid + residual streams. Returns 0 on success.
 * The two streams are stored separately, NOT interleaved: interleaving
 * would let one changed residual break the chunk holding the grid values
 * next to it, which is the whole thing we are trying to avoid. */
int  gn_quant_encode(const gn_mesh *m, float step, gn_quant_mode mode,
                     gn_quantized *out);

/* Rebuild the mesh. In GN_QUANT_EXACT this is bit-identical to the input. */
int  gn_quant_decode(const gn_quantized *q, gn_mesh *out);

void gn_quant_free(gn_quantized *q);

/* Largest absolute coordinate error between two meshes, and the index of
 * the worst vertex. Returns -1 if the meshes differ in size. */
double gn_mesh_max_error(const gn_mesh *a, const gn_mesh *b, uint32_t *worst);

/* ---- scene: assets + instances --------------------------------------- */

#define GN_INSTANCE_BYTES 68u    /* u32 asset id + 16 float matrix */

typedef struct gn_scene gn_scene;

gn_scene *gn_scene_new(gn_engine *e);
void      gn_scene_free(gn_scene *s);

/* Store geometry once. Returns the asset id, or (uint32_t)-1 on failure. */
uint32_t  gn_scene_add_asset(gn_scene *s, const char *name,
                             const gn_quantized *q);

/* Place an asset. `m` is a 16-float row-major affine matrix. */
uint32_t  gn_scene_add_instance(gn_scene *s, uint32_t asset, const float m[16]);

/* Move/rotate/scale an existing instance. This is the operation the whole
 * design exists for: it rewrites GN_INSTANCE_BYTES bytes and touches no
 * geometry at all. Returns 0 on success. */
int       gn_scene_set_transform(gn_scene *s, uint32_t instance, const float m[16]);

/* Convenience: translate an instance by (dx,dy,dz), preserving the rest. */
int       gn_scene_translate(gn_scene *s, uint32_t instance,
                             float dx, float dy, float dz);

int       gn_scene_get_transform(const gn_scene *s, uint32_t instance, float m[16]);

uint32_t  gn_scene_instances(const gn_scene *s);
uint32_t  gn_scene_assets(const gn_scene *s);
gn_object *gn_scene_instance_object(const gn_scene *s);

/* Identity matrix helper. */
void      gn_mat_identity(float m[16]);
void      gn_mat_translation(float m[16], float x, float y, float z);

#ifdef __cplusplus
}
#endif
#endif /* GENNA_SCENE_H */
