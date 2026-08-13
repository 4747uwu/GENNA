"""Does Genna need delta-encoded chunks? Ask git, fairly.

Genna's chunk store is content-addressed, which is exact-match: a one-line edit
inside a 4 KB chunk makes a chunk 99.9% identical to one already stored and
sharing nothing with it. git does not have that problem -- packfiles delta
objects against similar objects. That is the case for adding delta encoding.

But the comparison only means something if git is allowed to do its best work.
Measuring git with auto-gc disabled measures loose objects, where every commit
writes a whole new zlib-compressed blob -- which flatters Genna enormously and
proves nothing about whether delta encoding is worth building.

So this measures git three ways on the identical workload:
  * loose      -- as committed, no repack
  * repacked   -- after `git gc --aggressive`, i.e. delta compression ON
  * and Genna's saved-store growth on exactly the same edits, for reference.

Same corpus, same 100 insertions, same byte positions.
"""
import os, shutil, subprocess, sys, time

REPO_EDITS = 100


def sh(args, cwd, check=True):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(args)}\n{r.stdout}\n{r.stderr}")
    return r


def du(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def main():
    corpus = sys.argv[1]
    work = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else REPO_EDITS

    with open(corpus, "rb") as fh:
        data = fh.read()
    clen = len(data)
    print(f"=== git delta compression on the same workload ===")
    print(f"   corpus: {corpus} ({clen/1048576:.2f} MB), commits: {n}\n")

    repo = os.path.join(work, "gitdelta")
    shutil.rmtree(repo, ignore_errors=True)
    os.makedirs(repo)
    sh(["git", "init", "-q", "."], repo)
    for k, v in (("user.email", "b@b"), ("user.name", "b"),
                 ("commit.gpgsign", "false"), ("gc.auto", "0")):
        sh(["git", "config", k, v], repo)

    target = os.path.join(repo, "src.txt")
    with open(target, "wb") as fh:
        fh.write(data)
    sh(["git", "add", "-A"], repo)
    sh(["git", "commit", "-qm", "init"], repo)
    sh(["git", "gc", "-q", "--aggressive"], repo, check=False)
    g0 = du(os.path.join(repo, ".git"))
    print(f"   .git after packed baseline:   {g0:>12,} bytes")

    buf = bytearray(data)
    t0 = time.time()
    for i in range(n):
        line = f"\n// edit {i}\n".encode()
        at = (i * 7919) % (clen - 1)      # same positions deltabench uses
        buf[at:at] = line
        with open(target, "wb") as fh:
            fh.write(buf)
        sh(["git", "add", "src.txt"], repo)
        sh(["git", "commit", "-qm", f"edit {i}"], repo)
    tcommit = time.time() - t0

    g1 = du(os.path.join(repo, ".git"))
    print(f"   .git after {n} commits (loose): {g1:>12,} bytes"
          f"   -> {(g1-g0)/n:>10,.0f} bytes/commit")

    t0 = time.time()
    sh(["git", "gc", "-q", "--aggressive"], repo, check=False)
    tgc = time.time() - t0
    g2 = du(os.path.join(repo, ".git"))
    print(f"   .git after `git gc --aggressive`: {g2:>9,} bytes"
          f"   -> {(g2-g0)/n:>10,.0f} bytes/commit")
    print(f"\n   repack recovered {g1-g2:,} bytes ({g1/max(g2,1):.1f}x smaller), "
          f"took {tgc:.1f}s")
    print(f"   commit cost: {tcommit/n*1000:.0f} ms/commit")

    print("\n   --- what this decides -------------------------------------")
    print(f"   git, delta compression ON: {(g2-g0)/n:,.0f} bytes/commit")
    print("   Genna store growth on the same edits: run tools/run_deltabench.sh")
    print("   Delta-encoding Genna's chunks is worth building only if Genna's")
    print("   number is materially worse than git's repacked number.")

    shutil.rmtree(repo, ignore_errors=True)


if __name__ == "__main__":
    main()
