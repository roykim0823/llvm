class Inst<string mnemonic, int opcode> {
  string Mnemonic = mnemonic;
  int Opcode = opcode;
}

class InstDesc<string name, string suffix, int delta> {
  string Name = name;
  string Suffix = suffix;
  int Delta = delta;
}

// Define records for each operand type, with the appropriate suffix and opcode delta.
def RegOp   : InstDesc<"", "", 0>;
def ImmOp   : InstDesc<"I", "i", 1>;
def MemOp   : InstDesc<"M", "m", 2>;

class getDesc<int n> {
  // Use a conditional to select the appropriate InstDesc based on the value of n.
  // it works like a switch statement, but it is more concise and easier to read.
  InstDesc ret = !cond(!eq(n, 0) : RegOp,
                       !eq(n, 1) : ImmOp,
                       !eq(n, 2) : MemOp);
}

multiclass InstWithOps<string mnemonic, int opcode> {
  foreach I = 0-2 in {
    defvar Name = getDesc<I>.ret.Name;
    defvar Suffix = getDesc<I>.ret.Suffix;
    defvar Delta = getDesc<I>.ret.Delta;
    def Name  : Inst<mnemonic # Suffix, !add(opcode, Delta)>;
  }
}

multiclass InstWithOpsv<string mnemonic, int opcode> {
  // loop over a list of elements
  foreach I = [RegOp, ImmOp, MemOp] in {
    defvar Name = I.Name;
    defvar Suffix = I.Suffix;
    defvar Delta = I.Delta;
    def Name  : Inst<mnemonic # Suffix, !add(opcode, Delta)>;
  }
}

defm ADD : InstWithOps<"add", 0xA0>;
defm ADDv : InstWithOpsv<"addv", 0xA0>;