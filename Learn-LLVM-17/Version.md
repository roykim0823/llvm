# LLVM Verion 21 compatibility

```
LLVM version: 21.1.8
```

- Chapter02: O
- Chapter03: with `#if __clang_major__` macro
- Chapter04: with `#if __clang_major__` macro
- Chapter05: with `#if __clang_major__` macro
- Chapter06: ?
- Chapter07: ?
- Chapter08: with the following changes,
```
TableGen.cpp:bool Main(raw_ostream &OS, const RecordKeeper &Records) {  // const RecordKeeper & in llvm21
TableGen.cpp:  // llvm_shutdown_obj Y;  // Automatically call llvm_shutdown() on exit in llvm21
TokenEmitter.cpp:  // Records.startTimer("Emit flags");  // Not in llvm21
TokenEmitter.cpp:  for (const Record *CC :  // const Record* in llvm21
TokenEmitter.cpp:  std::vector<const Record *> AllTokenFilter =  // const Record* in llvm21
TokenEmitter.cpp:  const ListInit *TokenFilter = dyn_cast_or_null<ListInit>(  // const ListInit* in llvm21
TokenEmitter.cpp:   const Record *CC = TokenFilter->getElementAsRecord(I);  // const Record* in llvm21
TokenEmitter.cpp:   const ListInit *Flags = nullptr;  // const ListInit* in llvm21
TokenEmitter.cpp:   if (const RecordVal *F = CC->getValue("Flags"))  // const RecordVal* in llvm21
```
- Chapter09: ?
- Chapter10: ?
- Chapter11~13: No build required