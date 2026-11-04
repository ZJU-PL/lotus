

首先给函数指针赋值存在3种情况：

- case1:直接用函数指针给field赋值：这个在test1里面得到了体现。

```cpp
typedef void (*fptr_t)(char *, char *);
struct A { fptr_t handler; };
struct B { struct A a; }; // B is an outer layer of A

// Store functions with initializers
struct B b = { .a = { .handler = &copy_with_check } };
```

- case2:`ptrtoint`-首先将函数指针cast到 `intptr_t` 或者 `uintptr_t` 类型，在test5里面得到体现，
这类case在实际调用indirect-call的时候会先将call expression用 `inttoptr` 指令进行cast。

```cpp
typedef void (*fptr_t)(char *, char *);
struct A { intptr_t handler; };
struct B { struct A a; }; // B is an outer layer of A

// Store functions with initializers
struct B b = { .a = { .handler = (intptr_t)copy_with_check } };
```

- case3:`bitcast`-首先将函数指针cast到 `void*` 或者 `char*` 等其它指针类型，在test6只能够得到体现。
通常调用的时候也会先用 `bitcast` 指令将 `void*` 等指针类型cast到函数指针类型

```cpp
typedef void (*fptr_t)(char *, char *);
struct A { void* handler; };
struct B { struct A a; }; // B is an outer layer of A

// Store functions with initializers
struct B b = { .a = { .handler = (void*)copy_with_check } };
```

# 1.全局变量约束分析

全局变量约束分析主要分析全局变量的initializer，采用BFS算法对聚合常量层次分析。
对应 `MLTA::typeConfineInInitializer` 函数。
比如 `{{1,...}, {2,...}}` 会从顶层常量BFS遍历到2个子聚合常量。遍历每个单独的子常量时考虑下面几种情况：

- case1: 此时直接标记对应function为 `FoundF`

- case2: 这里需要获取 `ptrtoint` 指令的操作数，如果是函数指针那么标记为 `FoundF`。

- case3: 需要首先获取 `bitcast` 指令的操作数，如果是函数指针那么标记为 `FoundF`。

- case4:除了处理上述3种情况，在遍历子常量时还有可能出现子常量中出现指向其它复杂数据类型全局变量的指针，这个在test4得到体现，
跟之前的区别在于之前多层结构体是直接包含变量本身而不是用指针指向下层结构体。
对于这类case，MLTA会接着去访问对应下层结构体变量的initializer， 同时把下层结构体类型添加进 `typeCapSet` 集合。
比如下面示例中在访问 `b` 的initializer时遇到 `ba` 的指针随后访问 `ba` 的initializer，同时 `struct A` 会被添加进 `typeCapSet`。

```cpp
typedef void (*fptr_t)(char *, char *);
struct A { fptr_t handler; };
struct B { struct A* a; }; // B is an outer layer of A

struct A ba = { .handler = &copy_with_check };
// Store functions with initializers
struct B b = { .a = &ba };
```

最后在每层结构体如果找到了 `FoundF`，那么获取当前结构体类型层次并进行约束分析。
比如 `struct B b = { .a = { .handler = &copy_with_check } };` 中，
找到 `copy_with_check` 时解析出当前类型层次包括 `struct A` 的第0个field `handler` 和 `struct B` 的第0个field `a`，
那么执行 `typeIdxFuncsMap[A][0].insert(copy_with_check)` 以及 `typeIdxFuncsMap[B][0].insert(copy_with_check)`。


需要注意的是这里的全局变量不仅包括用户自定义的全局变量，还可能包括中间生成的全局变量、常量。

# 2.函数内约束分析

## 2.1.收集当前函数类型别名

对应 `MLTA::collectAliasStructPtr` 函数。

收集Function F中的cast操作，主要收集 `char*`, `void*` 到其它复合结构指针类型的cast操作。
记录转换前后变量的映射，存放在 `AliasStructPtrMap` 变量中。

## 2.2.约束分析

对应 `MLTA::typeConfineInFunction` 函数。

函数内约束分析主要考虑3种情况：

- 1.函数内 `store` 指令：对于 `store <value>, <ptr>` 这样的指令， 
  如果 `<value>` 是address-taken function，那么将 `<value>` 约束到 `<ptr>` 对应的类型上。

- 2.函数内indirect-call指令

- 3.函数内direct-call指令：假设某个call指令调用函数 `func`，
  且 `func` 第i个参数为address-taken function。那么查看 `func` 的函数体，
  找出用到第i个参数的 `store` 指令和 `BitCast` 指令，将对应address-taken function约束到对应变量的类型上。


将Function F约束到变量V上的操作对应 `MLTA::confineTargetFunction` 函数，即：

- 解析变量 `V` 的结构体类型层次。对应 `MLTA::getBaseTypeChain` 函数。

- `typeIdxFuncsMap[type][field_idx].insert(FoundF);`。


分析时会忽略llvm intrinsic function。
`Function::isIntrinsic` 方法返回该function是不是LLVM的intrinsic function，比如是不是 `llvm.` 开头的函数


## 2.3.类型传播分析

对应 `MLTA::typePropInFunction` 函数，需要注意的是类型传播并不局限于cast，还包括field之间的赋值。比如：
`a.f1 = b.f2;`，那么在访问 `a.f1` 下的其它field时，也同时得考虑 `b.f2` 下的其它field。

类型传播有3种case，前两种case对应上面field之间赋值的情况，最后一种为cast操作。
test8反映了前2种case

- 1.`store` 指令: `store <ptr>, <value>` 中 `<ptr>` 和 `<value>` 的base type不一定一致。
这时存在 `value_type` -> `ptr_type` 的类型转换。

- 2.llvm内置函数 `llvm.memcpy.p0i8.p0i8.i64` 的执行，这类case test2中有体现，这类case调用llvm内置函数批量内存拷贝，
可能在出现聚合常量赋值的时候出现。这时存在从 `source_type` -> `dest_type` 的转换。

```cpp
void scene2_a () {
    struct S s = {&f2 , 0};
    scene2_b(&s);
}
```

```bash
@__const.scene2_a.s = private unnamed_addr constant %struct.S { void (i64)* @f2, void (i32)* null }, align 8
call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %0, i8* align 8 bitcast (%struct.S* @__const.scene2_a.s to i8*), i64 16, i1 false), !dbg !68
call void @scene2_b(%struct.S* %s), !dbg !69
```

- 3.`bitcast` 指令

类型转换涉及两个层面，一个是结构体的某个field被转换到某个类型，一个是某个类型被转换到另一个类型，这部分结果保存在 `typeIdxPropMap` 中。

其中 `typeIdxPropMap[ToType][field_idx1] -> (FromType, field_idx2)` 表示 `(ToType, field_idx1)` 下包含从 `(FromType, field_idx2)` 转化过来的field。
`field_idx2 = -1` 直接从 `FromType` 转化，没有field访问。`field_idx1 = -1` 表示直接转化为 `ToType`。


# 3.间接调用分析

对应 `MLTA::findCalleesWithMLTA` 函数。

对于一个callee expression，MLTA会求出该callee expression对应的type-chain，以及dependent type对应的type chain，并计算target set。
target采用交集运算。

比如 `var.f1.func(..)`，对应的type-chain为 `(type(var), idx(f1)) --> (type(type(var).f1), idx(..func))`，
则在 `typeIdxFuncsMap` 查询对应两个typeidx的target set取交集。

至于 dependent type, MLTA会分别通过BFS查询这两个type idx依赖的其它type idx。
所谓依赖即 `(FromType, FromIdx) --> (ToType, ToIdx)` 则 `(ToType, ToIdx)` 依赖于 `(FromType, FromIdx)`。

求出dependent type后也会做同样的操作。