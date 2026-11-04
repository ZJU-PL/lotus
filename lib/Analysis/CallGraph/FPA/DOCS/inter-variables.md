

MLTA中涉及到的中间变量包括：

- `set<size_t> typeCapSet;`: 保存escape type的hash值

- `set<size_t> typeEscapeSet;`: 保存escape type，和 `typeCapSet` 的区别在于是field-sensitive的。
不过和前者一样在默认编译选项中貌似没有用到。

- `map<Function*, map<Value*, Value*>> AliasStructPtrMap;`: 保存一个function F中涉及到类型转换的指令。
比如在当前function中，有一个指令 `b% = bitcast ty1 %a ty2`，将 `ty1` 类型的变量 `a` 转化为 `ty2` 类型的变量 `b`。
如果 `ty1` 和 `ty2` 满足下面关系，那么将 `a` -> `b` 添加进 `AliasStructPtrMap[F]`。即保存 `void*` 转化为符合数据类型指针的指令

    * `ty1` 必须是 `void*`, `char*` 等8字节类型指针，即base指针类型。

    * `ty2` 必须是结构体、数组、`vector` 3种复杂数据类型的指针之一。

- `DenseMap<size_t, map<int, FuncSet>> typeIdxFuncsMap;`: 
`typeIdxFuncsMap[ty][field_idx]` 保存结构体类型 `ty` 的第 `field_idx` 个 `field` 覆盖到的address-function集合。

- `map<size_t, map<int, set<pair<size_t, int>>>> typeIdxPropMap;`: 保存结构体field之间或者类型之间的cast关系，
`typeIdxFuncsMap[ToTy][field_idx1] = {(FromTy, field_idx2)}` 表示 `FromTy` 的第 `field_idx2` 个field
被赋值给过 `ToTy` 的第 `field_idx1` 个field。
如果 `field_idxi = -1` 表示是普通的类型转换，没有field访问。类型转换可以包括显示类型转换和隐式类型转换。
如果 `a.f1 = b.f2`，则说明 `b.f2` 的类型被转换到 `a.f1` 的类型。分析 `a.f1` 包括的function set时也需要包括 `b.f2` 的function set。