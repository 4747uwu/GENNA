"""Real sentence embeddings — the seam that was marked
`[SEAM: REAL EMBEDDING MODEL]` in genna_curate.c.

What was there before was a hashed bag of word and character n-grams. That
catches typos, spacing and case, and it was honest about being lexical. It
cannot catch a paraphrase, because a paraphrase shares no n-grams. An ML team
evaluating near-dedup will throw paraphrases at it in the first five minutes.

This runs `sentence-transformers/all-MiniLM-L6-v2` — the actual model, the
actual weights — through **onnxruntime**, not torch. That matters: torch is a
2 GB dependency and the reason a lot of people never get to the try. The ONNX
route is ~90 MB and imports in under a second.

Pooling and normalization here reproduce what sentence-transformers does for
this model (mean pooling over the attention mask, then L2 normalize), so the
vectors are the same ones you would get from
`SentenceTransformer("all-MiniLM-L6-v2").encode(...)`, up to float error.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Sequence

__all__ = ["Embedder", "DEFAULT_MODEL", "available"]

DEFAULT_MODEL = "sentence-transformers/all-MiniLM-L6-v2"

#: Repo-relative candidates for the ONNX file inside a HF model repo.
_ONNX_CANDIDATES = ("onnx/model.onnx", "model.onnx", "onnx/model_O2.onnx")

_MISSING = (
    "Semantic near-dedup needs the optional extras:\n"
    '    pip install "genna[semantic]"\n'
    "(onnxruntime + tokenizers + huggingface-hub; no torch required)"
)


def available() -> bool:
    """True if the semantic extras are importable."""
    try:
        import numpy, onnxruntime, tokenizers  # noqa: F401
        return True
    except Exception:
        return False


class Embedder:
    """Sentence embeddings via ONNX. Thread-safe for `encode`.

    The model is downloaded once to the standard HuggingFace cache and reused.
    Point `GENNA_EMBED_MODEL` at another repo id to swap models, or pass
    `local_files_only=True` to forbid network access.
    """

    def __init__(self, model: str | None = None, *, local_files_only: bool = False,
                 max_length: int = 256, providers: Sequence[str] | None = None):
        try:
            import numpy as np
            import onnxruntime as ort
            from tokenizers import Tokenizer
        except ImportError as e:                       # pragma: no cover
            raise ImportError(f"{_MISSING}\n(missing: {e.name})") from None

        self._np = np
        self.name = model or os.environ.get("GENNA_EMBED_MODEL") or DEFAULT_MODEL
        self.max_length = max_length

        onnx_path, tok_path = self._fetch(self.name, local_files_only)

        so = ort.SessionOptions()
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        # Deterministic and polite by default: one thread per session, so
        # embedding a dataset does not fight the rest of a training box.
        so.intra_op_num_threads = int(os.environ.get("GENNA_EMBED_THREADS", "0")) or 0
        self._sess = ort.InferenceSession(
            str(onnx_path), sess_options=so,
            providers=list(providers) if providers else ["CPUExecutionProvider"])

        self._tok = Tokenizer.from_file(str(tok_path))
        self._tok.enable_truncation(max_length=max_length)
        self._tok.enable_padding(length=None)          # pad per batch

        self._input_names = {i.name for i in self._sess.get_inputs()}

    # ------------------------------------------------------------------
    @staticmethod
    def _fetch(repo: str, local_only: bool):
        try:
            from huggingface_hub import hf_hub_download
        except ImportError:                             # pragma: no cover
            raise ImportError(_MISSING) from None

        # A local directory works too, for air-gapped use.
        p = Path(repo)
        if p.is_dir():
            for cand in _ONNX_CANDIDATES:
                if (p / cand).exists():
                    return p / cand, p / "tokenizer.json"
            raise FileNotFoundError(f"no ONNX model found under {p}")

        last = None
        onnx_path = None
        for cand in _ONNX_CANDIDATES:
            try:
                onnx_path = hf_hub_download(repo_id=repo, filename=cand,
                                            local_files_only=local_only)
                break
            except Exception as e:                      # try the next layout
                last = e
        if onnx_path is None:
            raise RuntimeError(
                f"Could not fetch an ONNX model from {repo!r}. Tried "
                f"{_ONNX_CANDIDATES}. Last error: {last}")
        tok_path = hf_hub_download(repo_id=repo, filename="tokenizer.json",
                                   local_files_only=local_only)
        return onnx_path, tok_path

    # ------------------------------------------------------------------
    def encode(self, texts: Sequence[str], batch_size: int = 64,
               show_progress: bool = False):
        """Embed texts -> float32 array of shape (n, dim), L2-normalized."""
        np = self._np
        if not texts:
            return np.zeros((0, 384), dtype=np.float32)

        out = []
        total = len(texts)
        for start in range(0, total, batch_size):
            batch = [t if isinstance(t, str) else str(t)
                     for t in texts[start:start + batch_size]]
            # tokenizers pads to the longest in the batch
            encs = self._tok.encode_batch(batch)
            ids = np.array([e.ids for e in encs], dtype=np.int64)
            mask = np.array([e.attention_mask for e in encs], dtype=np.int64)

            feed = {}
            if "input_ids" in self._input_names:
                feed["input_ids"] = ids
            if "attention_mask" in self._input_names:
                feed["attention_mask"] = mask
            if "token_type_ids" in self._input_names:
                feed["token_type_ids"] = np.zeros_like(ids)

            hidden = self._sess.run(None, feed)[0]        # (b, seq, dim)

            # mean-pool over real tokens only, then L2 normalize -- this is
            # what sentence-transformers does for this model
            m = mask.astype(np.float32)[..., None]
            summed = (hidden * m).sum(axis=1)
            counts = np.clip(m.sum(axis=1), 1e-9, None)
            vecs = summed / counts
            norms = np.linalg.norm(vecs, axis=1, keepdims=True)
            vecs = vecs / np.clip(norms, 1e-12, None)
            out.append(vecs.astype(np.float32))

            if show_progress:
                done = min(start + batch_size, total)
                print(f"\r  embedding {done}/{total}", end="", flush=True)
        if show_progress:
            print()
        return np.vstack(out)

    # ------------------------------------------------------------------
    def similarity(self, a: str, b: str) -> float:
        v = self.encode([a, b])
        return float(v[0] @ v[1])

    def duplicate_indices(self, vecs, threshold: float = 0.85,
                          block: int = 1024) -> list[int]:
        """Indices to drop: each is >= threshold cosine to an EARLIER kept one.

        Blocked matrix multiply rather than the O(n^2) Python double loop the C
        tool used, so this stays usable past a few thousand records. Still
        quadratic in the worst case; an ANN index is the scale path and is not
        built here.
        """
        np = self._np
        n = len(vecs)
        if n < 2:
            return []
        kept: list[int] = []
        doomed: list[int] = []
        kept_vecs = np.zeros((0, vecs.shape[1]), dtype=np.float32)

        for start in range(0, n, block):
            chunk = vecs[start:start + block]
            # against everything kept so far
            if len(kept_vecs):
                sims = chunk @ kept_vecs.T
                hit_prev = sims.max(axis=1) >= threshold
            else:
                hit_prev = np.zeros(len(chunk), dtype=bool)

            # and against earlier members of this same chunk
            within = chunk @ chunk.T
            np.fill_diagonal(within, -1.0)
            keep_local = []
            for i in range(len(chunk)):
                if hit_prev[i]:
                    doomed.append(start + i)
                    continue
                if keep_local and (within[i, keep_local] >= threshold).any():
                    doomed.append(start + i)
                    continue
                keep_local.append(i)
            if keep_local:
                kept.extend(start + i for i in keep_local)
                kept_vecs = np.vstack([kept_vecs, chunk[keep_local]])
        return doomed
