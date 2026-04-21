import time

def bench():
    # ============================
    # TEST 1
    # ============================
    t0 = time.perf_counter()

    r00 = 10_000_000
    while r00:
        r00 -= 1

    t1 = time.perf_counter()

    # ============================
    # TEST 2 (sin máscara)
    # ============================
    r09 = 5_000_000
    r01, r02, r03, r04 = 1, 2, 3, 4

    while r09:
        r01 = r01 + r02
        r03 = r03 + r04
        r01 = r01 + r03
        r02 = r02 + r04
        r09 -= 1

    t2 = time.perf_counter()

    # ============================
    # TEST 3
    # ============================
    def trivial_func():
        pass

    r13 = 100_000
    while r13:
        trivial_func()
        r13 -= 1

    t3 = time.perf_counter()

    print(f"TEST 1 (loop simple): {(t1 - t0)*1000:.2f} ms")
    print(f"TEST 2 (ALU):         {(t2 - t1)*1000:.2f} ms")
    print(f"TEST 3 (call):        {(t3 - t2)*1000:.2f} ms")
    print(f"TOTAL:                {(t3 - t0)*1000:.2f} ms")

bench()