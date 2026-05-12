class Register<string n, int num> {
  string Name   = n;
  int    Number = num;
  bit    IsCallerSaved = 0;            // default
}

def R0 : Register<"r0", 0>;
def R1 : Register<"r1", 1>;
def R2 : Register<"r2", 2> { let IsCallerSaved = 1; }

class Inst<int opc, string mnem = "?", bit hasSideFx = 0> {
  int    Opcode    = opc;
  string Mnemonic  = mnem;
  bit    HasSideFx = hasSideFx;
}

def ADD : Inst<0x01, "add">;     // uses default HasSideFx = 0
def NOP : Inst<0x00>;            // uses two defaults
def OUT : Inst<0x10, "out", 1>;  // overrides everything
def DIV : Inst<opc=0x20, mnem="div">;     // named
def MUL : Inst<0x21, hasSideFx=1>;        // mix: positional then named

class Tagged {
  string MyName = NAME;
}
def Alpha : Tagged;     // MyName = "Alpha"
def Beta  : Tagged;     // MyName = "Beta"

// Exercise 1: Define a class `GPR` for general-purpose registers, with an integer template parameter `n` that is used to set the `AsmName` field to "x" followed by `n`. Then define some instances like `X0`, `X1`, etc. that use this class.

class GPR<int n> {
  string AsmName = "x" # n;   // string concatenation
}

def X0 : GPR<0>;     // AsmName = "x0"
def X1 : GPR<1>;     // AsmName = "x1"
def X2 : GPR<2>;     // AsmName = "x2"
def X3 : GPR<3>;     // AsmName = "x3"