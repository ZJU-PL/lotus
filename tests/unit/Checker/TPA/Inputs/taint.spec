SOURCE source Ret V T
SINK sink Arg0 V
SINK sink_mem Arg0 D
SOURCE fill Arg0 D T
PIPE copy Arg0 R Arg1 R
PIPE sum_bytes Ret V Arg0 R
PIPE combine Ret V AfterArg0 V
SOURCE taint_only_source Ret V T
IGNORE trusted_noop
PIPE format Arg0 D AfterArg1 V
PIPE format Arg0 D AfterArg1 D
