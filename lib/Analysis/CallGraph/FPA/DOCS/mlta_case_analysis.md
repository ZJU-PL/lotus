

# 1.type-escape分析

## 1.1.case1

以[mlta/test2](../testcases/mlta/test2/test.c)为例，用默认level（Os）编译时其scene2的IR为：

```asm
@__const.scene2_a.s = private unnamed_addr constant %struct.S { void (i64)* @f2, void (i32)* null }, align 8

; Function Attrs: noinline nounwind optnone ssp uwtable
define void @scene2_b(%struct.S* noundef %s) #0 !dbg !51 {
entry:
  %s.addr = alloca %struct.S*, align 8
  store %struct.S* %s, %struct.S** %s.addr, align 8
  call void @llvm.dbg.declare(metadata %struct.S** %s.addr, metadata !59, metadata !DIExpression()), !dbg !60
  %0 = load %struct.S*, %struct.S** %s.addr, align 8, !dbg !61
  %one = getelementptr inbounds %struct.S, %struct.S* %0, i32 0, i32 0, !dbg !62
  %1 = load void (i64)*, void (i64)** %one, align 8, !dbg !62
  call void %1(i64 noundef 0), !dbg !61
  ret void, !dbg !63
}

; Function Attrs: noinline nounwind optnone ssp uwtable
define void @scene2_a() #0 !dbg !64 {
entry:
  %s = alloca %struct.S, align 8
  call void @llvm.dbg.declare(metadata %struct.S* %s, metadata !65, metadata !DIExpression()), !dbg !66
  %0 = bitcast %struct.S* %s to i8*, !dbg !66
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %0, i8* align 8 bitcast (%struct.S* @__const.scene2_a.s to i8*), i64 16, i1 false), !dbg !66
  call void @scene2_b(%struct.S* noundef %s), !dbg !67
  ret void, !dbg !68
}
```

上面的case，在分析 `call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %0, i8* align 8 bitcast (%struct.S* @__const.scene2_a.s to i8*), i64 16, i1 false)` 这个语句时用到了
`llvm.memcpy.p0i8.p0i8.i64` 对应的传播规则，即 `*PO = VO` 中，`PO` （`%0`）为复合数据类型。此时 `MLTA::propagateType` 在调用 `MLTA::getBaseTypeChain` 计算其类型层次时，
追踪了 `%0 = bitcast %struct.S* %s to i8*` 和 `%s = alloca %struct.S, align 8` 2个指令，第一个指令对应 `MLTA::nextLayerBaseType` 中bitcast指令规则。
第2个指令 `alloca` 没有对应规则，因此 `NextV` 返回 `NULL`。`struct.S` 为escaped-type。

通过这个示例分析：凡是 `*PO = VO` (`store VO, PO`) 中 `*PO` 最终是 `alloca` 或者 `global.zeroinitializer` 对应类型都会被添加到 `typeCapSet`。
个人理解这个规则是为了应对下面这样的case。（[test13](../testcases/mlta/test13/test.c)就是一个例子）
这个case中 `b.a = &a;` 的存在使得无法分析 `B::a` 约束的function，因此要么 `B::a` 被添加到 `typeEscapedSet` 要么 `B` 被添加到 `typeCapSet`。
mlta的implementation选择了后者。

```cpp
struct A {
    fptr_t f;
};

struct B {
    struct A* a;
};

int main() {
    struct A a = {func};
    struct B b;
    b.a = &a;
}
```




## 1.2.case2

以[mlta/test11](../testcases/mlta/test11/test.c)为例，用默认level（Os）编译时其scene2的IR为：

```asm
; Function Attrs: noinline nounwind optnone ssp uwtable
define internal void @o_stream_default_set_flush_callback(%struct.ostream_private* noundef %_stream, i32 (i8*)* noundef %callback, i8* noundef %context) #0 !dbg !62 {
entry:
  %_stream.addr = alloca %struct.ostream_private*, align 8
  %callback.addr = alloca i32 (i8*)*, align 8
  %context.addr = alloca i8*, align 8
  store %struct.ostream_private* %_stream, %struct.ostream_private** %_stream.addr, align 8
  call void @llvm.dbg.declare(metadata %struct.ostream_private** %_stream.addr, metadata !63, metadata !DIExpression()), !dbg !64
  store i32 (i8*)* %callback, i32 (i8*)** %callback.addr, align 8
  call void @llvm.dbg.declare(metadata i32 (i8*)** %callback.addr, metadata !65, metadata !DIExpression()), !dbg !66
  store i8* %context, i8** %context.addr, align 8
  call void @llvm.dbg.declare(metadata i8** %context.addr, metadata !67, metadata !DIExpression()), !dbg !68
  %0 = load i32 (i8*)*, i32 (i8*)** %callback.addr, align 8, !dbg !69
  %1 = load %struct.ostream_private*, %struct.ostream_private** %_stream.addr, align 8, !dbg !70
  %callback1 = getelementptr inbounds %struct.ostream_private, %struct.ostream_private* %1, i32 0, i32 2, !dbg !71
  store i32 (i8*)* %0, i32 (i8*)** %callback1, align 8, !dbg !72
  %2 = load i8*, i8** %context.addr, align 8, !dbg !73
  %3 = load %struct.ostream_private*, %struct.ostream_private** %_stream.addr, align 8, !dbg !74
  %context2 = getelementptr inbounds %struct.ostream_private, %struct.ostream_private* %3, i32 0, i32 3, !dbg !75
  store i8* %2, i8** %context2, align 8, !dbg !76
  ret void, !dbg !77
}

; Function Attrs: noinline nounwind optnone ssp uwtable
define internal void @stream_send_io(%struct.ostream_private* noundef %stream) #0 !dbg !85 {
entry:
  %stream.addr = alloca %struct.ostream_private*, align 8
  store %struct.ostream_private* %stream, %struct.ostream_private** %stream.addr, align 8
  call void @llvm.dbg.declare(metadata %struct.ostream_private** %stream.addr, metadata !88, metadata !DIExpression()), !dbg !89
  %0 = load %struct.ostream_private*, %struct.ostream_private** %stream.addr, align 8, !dbg !90
  %callback = getelementptr inbounds %struct.ostream_private, %struct.ostream_private* %0, i32 0, i32 2, !dbg !91
  %1 = load i32 (i8*)*, i32 (i8*)** %callback, align 8, !dbg !91
  %2 = load %struct.ostream_private*, %struct.ostream_private** %stream.addr, align 8, !dbg !92
  %context = getelementptr inbounds %struct.ostream_private, %struct.ostream_private* %2, i32 0, i32 3, !dbg !93
  %3 = load i8*, i8** %context, align 8, !dbg !93
  %call = call i32 %1(i8* noundef %3), !dbg !90
  ret void, !dbg !94
}

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @main() #0 !dbg !9 {
entry:
  %retval = alloca i32, align 4
  %stream = alloca %struct.ostream_private*, align 8
  %stream1 = alloca %struct.ostream_private*, align 8
  %context = alloca i8*, align 8
  store i32 0, i32* %retval, align 4
  call void @llvm.dbg.declare(metadata %struct.ostream_private** %stream, metadata !14, metadata !DIExpression()), !dbg !41
  call void @llvm.dbg.declare(metadata %struct.ostream_private** %stream1, metadata !42, metadata !DIExpression()), !dbg !43
  %0 = load %struct.ostream_private*, %struct.ostream_private** %stream1, align 8, !dbg !44
  %callback = getelementptr inbounds %struct.ostream_private, %struct.ostream_private* %0, i32 0, i32 2, !dbg !45
  store i32 (i8*)* @iostream_pump_flush1, i32 (i8*)** %callback, align 8, !dbg !46
  call void @llvm.dbg.declare(metadata i8** %context, metadata !47, metadata !DIExpression()), !dbg !48
  %1 = load %struct.ostream_private*, %struct.ostream_private** %stream, align 8, !dbg !49
  %2 = load i8*, i8** %context, align 8, !dbg !50
  call void @o_stream_default_set_flush_callback(%struct.ostream_private* noundef %1, i32 (i8*)* noundef @iostream_pump_flush, i8* noundef %2), !dbg !51
  %3 = load %struct.ostream_private*, %struct.ostream_private** %stream, align 8, !dbg !52
  call void @stream_send_io(%struct.ostream_private* noundef %3), !dbg !53
  ret i32 0, !dbg !54
}
```

对于上面的例子，原版MLTA算法implementation没有考虑到 `_stream->callback = callback;` 中 `_stream->callback` 会escape，因此没有分析出 `stream->callback(stream->context);` 的callee set。
对于这个case，我们认为，当 `_stream->callback = callback;` 的 `callback` 不是常量时，应该将 `_stream->callback` 添加到 `typeEscapeSet` 中。
考虑到 `callback` 背后可能涉及到complex data-flow。比如被数组元素赋值或者通过call-chain赋值（call-chain有可能涉及到别的间接调用）。


