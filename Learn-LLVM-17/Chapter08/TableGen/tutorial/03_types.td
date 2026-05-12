class Anything;
def  X : Anything;
def  Y : Anything;

def TypeShowcase {
  bit        Flag   = 1;
  int        N      = -42;
  bits<8>    Opcode = 0b00010110;       // 8 bits
  string     S      = "hello";
  string     Big    = "abc"  "def";     // adjacent literals concatenate -> "abcdef"
  code       Body   = [{
    return x + 1;
  }];
  list<int>  Primes = [2, 3, 5, 7];
  list<list<int>> Matrix = [[1,2],[3,4]];
  Anything   Ref    = X;                // a typed record reference
  list<Anything> Refs = [X, Y];
}

def Demo {
  bits<8> Op  = 0b10110010;
  bit     B0  = Op{0};        // -> 0   (least significant)
  bit     B7  = Op{7};        // -> 1
  bits<4> Top = Op{7...4};    // -> { 1, 0, 1, 1 }
}

def Words {
  bits<16> W0 = 0x1234;
  bit B6 = W0{6};          // -> 1
  bit B10 = W0{10};        // -> 0
}
