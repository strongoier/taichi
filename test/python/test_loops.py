import taichi_lang as ti
from pytest import approx

def test_loops():
  for arch in [ti.x86_64, ti.cuda]:
    ti.reset()
    ti.cfg.arch = arch
    x = ti.var(ti.f32)
    y = ti.var(ti.f32)

    N = 512
    @ti.layout
    def place():
      ti.root.dense(ti.i, N).place(x)
      ti.root.dense(ti.i, N).place(y)
      ti.root.lazy_grad()

    for i in range(N // 2, N):
      y[i] = i - 300

    @ti.kernel
    def func():
      for i in range(N // 2 + 3, N):
        x[i] = ti.abs(y[i])

    func()

    for i in range(N // 2 + 3):
      assert x[i] == 0

    for i in range(N // 2 + 3, N):
      assert x[i] == abs(y[i])

test_loops()
