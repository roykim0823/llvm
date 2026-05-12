class Inst { bit hasSideFx = 0; int Latency = 1; }

def LOAD : Inst {
  let hasSideFx = 1;
  let Latency = 4;
}

let hasSideFx = 1, Latency = 8 in {
  def LOAD8  : Inst;
  def STORE8 : Inst;
}

// TableGen has no `let append`/`let prepend`. Use !listconcat — and because
// `let items = f(items)` self-references the field being assigned, the clean
// idiom is to pass the extras as class template parameters.
// class Base   { list<int> items = [2, 3]; }
// class Middle : Base   { let append  items = [4]; }   // -> [2, 3, 4]
// def   Final  : Middle { let prepend items = [1]; }   // -> [1, 2, 3, 4]

class Base { list<int> items = [2, 3]; }
class WithExtras<list<int> pre, list<int> post> {
  list<int> items = !listconcat(pre, !listconcat([2, 3], post));
}

def Appended  : WithExtras<[],  [4]>;         // -> [2, 3, 4]
def Prepended : WithExtras<[1], []>;          // -> [1, 2, 3]
def Both      : WithExtras<[1], [4]>;         // -> [1, 2, 3, 4]
def Override  : Base { let items = [10, 20, 30]; }   // plain override

class Op { bits<8> Opcode = 0; }

def ADD : Op {
  let Opcode{7-4} = 0b0010;   // upper nibble
  let Opcode{3-0} = 0b1100;   // lower nibble
}