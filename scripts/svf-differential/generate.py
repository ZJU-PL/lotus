#!/usr/bin/env python3
"""Generate deterministic, initialized pointer programs in LLVM 14 typed IR.

The five families exercise straight-line updates, diamonds, loops, direct
calls, and indirect calls. Odd seeds use struct fields as pointer slots.
These inputs are differential tests, not an execution or soundness oracle.
"""
import random


def generate(seed):
    rng = random.Random(seed)
    count = rng.randrange(2, 6)
    objects = [f"@o{i}" for i in range(count)]
    slots = [f"%s{i}" for i in range(count)]
    lines = [f"; generator seed {seed}, family {seed % 5}",
             'target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"']
    lines += [f"{obj} = global i8 0" for obj in objects]
    lines += ["define void @put(i8** %slot, i8* %value) {",
              "  store i8* %value, i8** %slot", "  ret void", "}",
              "define void @noop(i8** %slot, i8* %value) { ret void }",
              "define i32 @main(i32 %argc, i8** %argv) {", "entry:",
              "  %cond = icmp sgt i32 %argc, 1"]
    if seed % 2:
        ty = "{" + ", ".join(["i8*"] * count) + "}"
        lines.append(f"  %record = alloca {ty}")
        lines += [f"  {slot} = getelementptr {ty}, {ty}* %record, i32 0, i32 {i}"
                  for i, slot in enumerate(slots)]
    else:
        lines += [f"  {slot} = alloca i8*" for slot in slots]
    lines += [f"  store i8* {rng.choice(objects)}, i8** {slot}" for slot in slots]
    pointers, addresses = objects[:], slots[:]
    for i in range(rng.randrange(10, 35)):
        op = rng.randrange(4)
        if op == 0:
            lines.append(f"  store i8* {rng.choice(pointers)}, i8** {rng.choice(addresses)}")
        elif op == 1:
            name = f"%v{i}"
            lines.append(f"  {name} = load i8*, i8** {rng.choice(addresses)}")
            pointers.append(name)
        elif op == 2:
            name = f"%p{i}"
            lines.append(f"  {name} = select i1 %cond, i8** {rng.choice(addresses)}, i8** {rng.choice(addresses)}")
            addresses.append(name)
        else:
            name = f"%v{i}"
            lines.append(f"  {name} = select i1 %cond, i8* {rng.choice(pointers)}, i8* {rng.choice(pointers)}")
            pointers.append(name)
    family = seed % 5
    if family == 1:
        lines += ["  br i1 %cond, label %left, label %right", "left:",
                  f"  store i8* {rng.choice(pointers)}, i8** {rng.choice(addresses)}",
                  "  br label %join", "right:",
                  f"  store i8* {rng.choice(pointers)}, i8** {rng.choice(addresses)}",
                  "  br label %join", "join:"]
    elif family == 2:
        lines += ["  br label %loop", "loop:",
                  "  %i = phi i32 [0, %entry], [%next, %loop]",
                  f"  %loopvalue = load i8*, i8** {rng.choice(addresses)}",
                  f"  store i8* %loopvalue, i8** {rng.choice(addresses)}",
                  "  %next = add i32 %i, 1", "  %again = icmp ult i32 %next, 3",
                  "  br i1 %again, label %loop, label %exit", "exit:"]
    elif family == 3:
        lines.append(f"  call void @put(i8** {rng.choice(addresses)}, i8* {rng.choice(pointers)})")
    elif family == 4:
        lines += ["  %fp = select i1 %cond, void (i8**, i8*)* @put, void (i8**, i8*)* @noop",
                  f"  call void %fp(i8** {rng.choice(addresses)}, i8* {rng.choice(pointers)})"]
    lines += [f"  %r{i} = load i8*, i8** {slot}" for i, slot in enumerate(slots)]
    lines += ["  ret i32 0", "}"]
    return "\n".join(lines) + "\n"
