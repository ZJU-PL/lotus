

from https://github.com/for-just-we/IndirectCallAnalyzer

基于[mlta](https://github.com/umnsec/mlta/tree/main)重构。

# 1.Basic Usage

编译benchmark：确保安装LLVM，我们采用的是LLVM 15，编译benchmark时最好添加 `-g -Xclang -no-opaque-pointers -Xclang -disable-O0-optnone`。
一个是保留debug信息，一个是需要类型指针进行结构体分析，一个是 `mem2reg` 优化需要。

编译该project

```bash
mkdir build && cd build
cmake ..
cmake --build . -j 16
```

运行：`ica -output-file=cout -analysis-type=2 xxx.bc`

| 选项 | 说明 |
| ---- | ---- |
| `analysis-type` | 采用的分析算法，`1` 表示用 `FLTA`、`2` 表示用 `MLTA`、`3` 表示加强版 `MLTA`、`4` 表示 `Kelp` |
| `debug` | 是否输出运行时的debug信息 |
| `max-type-layer` | MLTA最大的类型匹配层数，默认 `10` |
| `output-file` | 间接调用分析结果输出的文件，没有默认不输出每个icall的target set，如果是 `cout` 那么用 `stdout` 输出，反之输出到指定文件。 |

# 2.LLVM Basis


## 2.1.basic

与source code不同的是，结构体initializer在global域和local域存在不同，以[test9](testcases/mlta/test9/test.c)为例。

其编译后生成的IR如下：

```asm
%struct.S = type { i32 (i32, i32)*, i32, i32 }

@s1 = global %struct.S { i32 (i32, i32)* @func1, i32 0, i32 1 }, align 8
@__const.main.s_local_1 = private unnamed_addr constant %struct.S { i32 (i32, i32)* @func1, i32 0, i32 1 }, align 8
@__const.main.s_local_2 = private unnamed_addr constant %struct.S { i32 (i32, i32)* @func2, i32 0, i32 2 }, align 8

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @func1(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %a.addr = alloca i32, align 4
  %b.addr = alloca i32, align 4
  store i32 %a, i32* %a.addr, align 4
  store i32 %b, i32* %b.addr, align 4
  %0 = load i32, i32* %a.addr, align 4
  %1 = load i32, i32* %b.addr, align 4
  %add = add nsw i32 %0, %1
  ret i32 %add
}

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @func2(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %a.addr = alloca i32, align 4
  %b.addr = alloca i32, align 4
  store i32 %a, i32* %a.addr, align 4
  store i32 %b, i32* %b.addr, align 4
  %0 = load i32, i32* %a.addr, align 4
  %1 = load i32, i32* %b.addr, align 4
  %sub = sub nsw i32 %0, %1
  ret i32 %sub
}

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @func3(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %a.addr = alloca i32, align 4
  %b.addr = alloca i32, align 4
  store i32 %a, i32* %a.addr, align 4
  store i32 %b, i32* %b.addr, align 4
  %0 = load i32, i32* %a.addr, align 4
  %1 = load i32, i32* %b.addr, align 4
  %mul = mul nsw i32 %0, %1
  ret i32 %mul
}

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @main() #0 {
entry:
  %retval = alloca i32, align 4
  %s_local_1 = alloca %struct.S, align 8
  %s_local_2 = alloca %struct.S, align 8
  %a = alloca i32, align 4
  %s_local4 = alloca %struct.S, align 8
  %s_local_3 = alloca %struct.S, align 8
  store i32 0, i32* %retval, align 4
  %0 = bitcast %struct.S* %s_local_1 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %0, i8* align 8 bitcast (%struct.S* @__const.main.s_local_1 to i8*), i64 16, i1 false)
  %1 = bitcast %struct.S* %s_local_2 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %1, i8* align 8 bitcast (%struct.S* @__const.main.s_local_2 to i8*), i64 16, i1 false)
  store i32 2, i32* %a, align 4
  %f = getelementptr inbounds %struct.S, %struct.S* %s_local4, i32 0, i32 0
  store i32 (i32, i32)* @func3, i32 (i32, i32)** %f, align 8
  %num = getelementptr inbounds %struct.S, %struct.S* %s_local4, i32 0, i32 1
  store i32 0, i32* %num, align 8
  %id = getelementptr inbounds %struct.S, %struct.S* %s_local4, i32 0, i32 2
  %2 = load i32, i32* %a, align 4
  store i32 %2, i32* %id, align 4
  %num1 = getelementptr inbounds %struct.S, %struct.S* %s_local_3, i32 0, i32 1
  store i32 ptrtoint (i32 (i32, i32)* @func3 to i32), i32* %num1, align 8
  ret i32 0
}
```

可以看到

- `main` 函数中的两个结构体变量 `s_local_1` 和 `s_local_2` 对应的initializer常量被转移到global域，因此MLTA分析时实际是在全局段进行分析，在local域，通过 `llvm.memcpy.p0i8.p0i8.i64` 函数将值转移到 `s_local_1, s_local_2`。

- 对于 `s_local4`，由于其initializer出现了局部变量，因此其field的初始化被拆分成一个个 `store` 指令。

    * `store i32 (i32, i32)* @func3, i32 (i32, i32)** %f, align 8` 对应 `s_local4.f = func3`。

因此在分析local域的type confinement时候不需要考虑initializer，主要考虑 `store` 指令。

同时可以看到 `Stu s_local4 = {func3, 0, a};` 中赋值操作是通过 `llvm.memcpy.p0i8.p0i8.i64` 完成的。
这个函数可以用来完成结构体赋值。
因此分析 `store` 的时候需要考虑这个函数。


在处理结构体层次时需要处理 `getelementptr` 指令，
代码里会跳过第一个index，其介绍在[what-is-the-first-index-of-the-gep-instruction](https://llvm.org/docs/GetElementPtr.html#what-is-the-first-index-of-the-gep-instruction)中，
所以field访问通常从第2个index开始。这里通过 `getElementType` API可访问子field的类型。



这里用MLTA分析中有非常多trick

- recover type: 在某些function中进行

## 2.2.类型系统


同时LLVM IR中判断类型相等也不是件容易的事，参考[cn-blog](https://blog.csdn.net/fcsfcsfcs/article/details/119062032)，[原版blog](https://lowlevelbits.org/type-equality-in-llvm/)。

比如，编译并链接完的nginx中发现同一个结构体可能链接后有多个不同的类型

```asm
%struct.ngx_http_phase_handler_s = type { i64 (%struct.ngx_http_request_s.1418*, %struct.ngx_http_phase_handler_s*)*, i64 (%struct.ngx_http_request_s.1418*)*, i64 }
%struct.ngx_http_phase_handler_s.2192 = type { i64 (%struct.ngx_http_request_s.2187*, %struct.ngx_http_phase_handler_s.2192*)*, i64 (%struct.ngx_http_request_s.2187*)*, i64 }
```

这些别名类型的存在严重影响mlta的效率。
对于这类同名类型我们分为两类：

- 1.在source code中只有1种实现

- 2.在source code中有多种实现

我们目前只考虑第一类情况。对于一个类型，如果它们去掉数字后缀后名称一样并且参数数量一样，我们认为是相同类型，并以此为基础进行hash。

## 2.3.链接后结构体变化

参考[TFA paper](https://www.usenix.org/system/files/usenixsecurity24-liu-dinghao-improving.pdf)、[why-a-function-pointer-field-in-a-llvm-ir-struct-is-replaced-by](https://stackoverflow.com/questions/18730620/why-a-function-pointer-field-in-a-llvm-ir-struct-is-replaced-by)、[Function pointer type becomes empty struct](https://lists.llvm.org/pipermail/cfe-dev/2016-November/051601.html)、[Function pointer type becomes empty struct](https://lists.llvm.org/pipermail/cfe-dev/2016-November/051633.html)、[Function pointer type becomes empty struct](https://lists.llvm.org/pipermail/cfe-dev/2016-November/051635.html)。
编译后结构体field访问和源代码可能对应不上导致MLTA发挥不了作用，TFA的解决思路是通过debug信息恢复原始类型信息，不过这一步我们这里并没有实现。

```cpp
/* 源代码 */
struct A {
 int i;
 int (*f)(int, struct A*);
 int (*g)(char, struct A*);
};

/* 预期对应IR */
%struct.A = type {i32, i32 (i32, %struct.A*), i32 (i8, %struct.A*)}

/* 实际IR */
%struct.A = type {i32, {}*, i32 (i8, %struct.A*)}
```


# 3.Implementations


## 3.1.MLTA

相对[原版MLTA](https://github.com/umnsec/mlta)，我们在实现上做了一些调整。 motivation参考[case study](docs/mlta_case_analysis.md)。

- 1.我们默认采用逐个比对类型的方式 (`MLTA::findCalleesWithType` 函数) 而不是比对函数签名的方式进行类型比对。

- 2.我们不将函数类型添加进 `typeCapSet` (`MLTAPass::confineTargetFunction` 函数最后一行)。

- 3.当 `store VO, PO` (`*PO = VO`) 时，假如 `VO` 不是常量，我们将 `PO` 涉及到的结构体类型层次添加进 `typeEscapedSet`。

除了上述3点，我们还注意到原版mlta以下问题：

- 1.当结构体类型被cast到 `void*` 或者整数类型时没有考虑


针对上面问题，我们的调整如下：

- 在 `MLTA::typePropInFunction` 函数的实现中，分析cast指令时如果识别到 `void` 或者整形指针与 `struct` 指针互相转换，将 `struct` 类型标记为escaped type。

除此之外，我们发现全局变量的 `initializer` 中存在 `g1 = { bitcast g2, ... }`，而之前的implementation没有考虑到 `bitcast` 和 `ptrtoint` 嵌套全局变量的情况。
碰到全局变量时会展开分析，造成递归分析的开销。因此我们在之前的implementation上补充了对 `bitcast` 和 `ptrtoint` 出现全局变量的处理。

此外，我们添加了一个 `MLTADFPass`，这个简单实现了TFA paper中的数据流分析策略，不过没有实现结构体信息恢复部分。

## 3.2.KELP

相比于paper，当前实现的KELP识别simple function pointer时在backward data flow分析时只考虑下面操作，
其中 `copy` 操作包括 `bitcast`, `ptrtoint`, `inttoptr` 指令以及 `bitcast` 和 `ptrtoint` operator。
由于KELP不考虑memory object，而top-level pointer都是SSA形式的，用 

- `pt(f)` 表示函数指针 `f` 的调用目标，对于 `l: f = fs`, `pt_l(f) = pt(f)`。

- `s(f)` 表示函数指针 `f` 是否为simple function pointer，这里用 `u(f)` 表示 `f` 所有的use点是否不包含memory object操作，如果 `u(f)` 为 `false`，那么 `s(f)` 一定为 `false`。


| 操作类型 | 规则 (建立在 `u(f) = true` 基础上) |
| ---- | ---- |
| `addr`: `f = &Func` | `pts(f) = { Func }`, `s(f) = true` |
| `copy`: `f = fs` | `pts(f) += pts(fs)`, `s(f) &= s(fs)` |
| `phi`: `f = phi(f1, f2)`| `pts(f) += pts(f1) + pts(f2)`, `s(f) &= s(f1) & s(f2)` |
| `arg`: `func(f, ..)` | for all call: `v = func(fa,..)`, `pts(f) += pts(fa)`, `s(f) &= s(fa)` |
| `call`: `f = getF(...)` | for return of `getF`, `ret fr`, `pts(f) += pts(fr)`, `s(f) &= s(fr)` |

Todo: 接着改进KELP分析规则，目前的KELP还没考虑函数指针数组、结构体变量、全局变量等复杂场景，以后会进一步优化分析规则。

## 3.3.Virtual Call

目前暂时跳过virtual call分析，关于virtual call的识别我们对SVF的策略进行了改进。
具体来说，SVF的策略基于以下pattern识别virtual call

```asm
%vtable = load this
%vfn = getelementptr %vtable, idx
%x = load %vfn
call %x (this)
```

一个示例是

```asm
%vtable = load void (%class.Base*)**, void (%class.Base*)*** %12, align 8
%vfn = getelementptr inbounds void (%class.Base*)*, void (%class.Base*)** %vtable, i64 0
%13 = load void (%class.Base*)*, void (%class.Base*)** %vfn, align 8
call void %13(%class.Base* noundef nonnull align 8 dereferenceable(16) %11)
```

这会将 `log_handler[i](r);` 这种模式的间接调用识别为virtual call。我们在此基础上对 `%vtable = load this` 加了一个规则。
就是判定 `%vtable` 对应的 `Value` 的 `name` 中是否包含 `vtable` 字符串，包括那么是virtual call，反之不是。
不过这可能需要编译时添加选项 `fno-discard-value names`。
除此之外也尝试过其它策略，但效果不是很好。

# 4.潜在bug

如果遇到exit code 139，很有可能是空指针访问。132应该是stack overflow，出现递归。

# 5.Test results

用了下面project来测试Kelp，编译选项为 `-g -fno-discard-value-names -fno-inline -fno-inline-functions -fno-optimize-sibling-calls -fno-unroll-loops -Xclang -disable-O0-optnone -Xclang -no-opaque-pointers`。
随后用 `opt -mem2reg xx.bc -o xx.bc` 优化。

下表表示用KELP分析时，其中的one layer call(只能用FLTA分析的call)、multi layer call(用MLTA分析的call)、simple call(KELP分析的call)数量。

| project | one layer call数量 | multi layer call数量 | simple indirect call数量 | confined function数量 | address-taken function数量 |
| ---- | ---- | ---- | ---- | ---- | ---- |
| bash-5.2 | 90 | 36 | 11 | 22 | 538 |
| git-2.47.0 | 120 | 385 | 84 | 181 | 1802 |
| htop-3.3.0 | 4 | 59 | 18 | 14 | 228 |
| nanomq-0.22.10 | 5 | 120 | 17 | 15 | 898 |
| nasm-2.16.03 | 6 | 76 | 0 | 0 | 183 |
| nginx-1.26.2 | 133 | 365 | 2 | 6 | 1446 |
| openssl-3.4.0 | 274 | 1635 | 90 | 197 | 6070 |
| perl-5.40.0 | 97 | 102 | 16 | 11 | 699 |
| php-8.3.13 | 900 | 1137 | 47 | 87 | 4654 |
| redis-7.4.1 | 48 | 378 | 3 | 7 | 741 |
| ruby-3.3.6 | 84 | 395 | 104 | 213 | 4137 |
| teeworlds-0.7.5 | 59 | 28 | 3 | 2 | 48 |
| tmux-3.5 | 22 | 59 | 7 | 6 | 653 |
| vim-9.1.08 | 937 | 97 | 11 | 14 | 1453 |
| wine-9.22 | 1 | 0 | 14 | 11 | 17 |

下表为每种方法求解的call targets总和（即所有indirect call的target size总和）

| project | FLTA | MLTA | Enhanced MLTA | KELP |
| ---- | ---- | ---- | ---- | ---- |
| bash-5.2 | 4588 | 3432 | 3382 | 2642 |
| git-2.47.0 | 30681 | 18016 | 16537 | 9541 |
| htop-3.3.0 | 1083 | 758 | 758 | 724 |
| nanomq-0.22.10 | 16357 | 7996 | 6705 | 6224 |
| nasm-2.16.03 | 1174 | 391 | 391 | 391 |
| nginx-1.26.2 | 28122 | 17354 | 17271 | 16749 |
| openssl-3.4.0 | 271838 | 210723 | 186782 | 173832 |
| perl-5.40.0 | 4943 | 3930 | 3723 | 3822 |
| php-8.3.13 | 319043 | 276003 | 268823 | 250423 |
| redis-7.4.1 | 6943 | 4927 | 4663 | 4452 |
| ruby-3.3.6 | 147531 | 105263 | 99273 | 47485 |
| teeworlds-0.7.5 | 2539 | 2194 | 2159 | 2159 |
| tmux-3.5 | 1685 | 1532 | 1163 | 1103 |
| vim-9.1.08 | 25656 | 24533 | 24479 | 22094 |
| wine-9.22 | 32 | 32 | 32 | 28 |


KELP分析callee为0的case大部分是由于调用链不完整，这是由于编译后有些caller function没被编译进二进制导致的。
或者获取函数指针时 `f = extfunc(...)`，通过调用外部函数获取 `f`，而外部函数未定义导致分析不完整。


# 6.Papers

> [[1.FLTA].Jinku Li, Xiaomeng Tong, Fengwei Zhang, and Jianfeng Ma. Fine-cfi: fine-grained control-flow integrity for operating system kernels. IEEE Transactions on Information Forensics and Security, 13(6):1535–1550, 2018.](https://cse.sustech.edu.cn/faculty/~zhangfw/paper/fine-cfi-tifs18.pdf)

> [[2.MLTA].Lu K, Hu H. Where does it go? refining indirect-call targets with multi-layer type analysis[C]//Proceedings of the 2019 ACM SIGSAC Conference on Computer and Communications Security. 2019: 1867-1881.](https://dl.acm.org/doi/pdf/10.1145/3319535.3354244)

> [[3.TFA].Liu D, Ji S, Lu K, et al. Improving {Indirect-Call} Analysis in {LLVM} with Type and {Data-Flow}{Co-Analysis}[C]//33rd USENIX Security Symposium (USENIX Security 24). 2024: 5895-5912.](https://www.usenix.org/system/files/usenixsecurity24-liu-dinghao-improving.pdf)

> [[4.KELP].Cai Y, Jin Y, Zhang C. Unleashing the Power of Type-Based Call Graph Construction by Using Regional Pointer Information[C]//33nd USENIX Security Symposium (USENIX Security 24). 2024](https://www.usenix.org/system/files/sec23winter-prepub-350-cai.pdf)