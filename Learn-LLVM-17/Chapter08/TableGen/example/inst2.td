class Inst<string mnemonic, int opcode> {
  string Mnemonic = mnemonic;
  int Opcode = opcode;
  bit Commutable = 0;
}

let Commutable = 1 in
def ADD1   : Inst<"add1", 0xA0>;

def ADD2   : Inst<"add2", 0xA1> {
  let Commutable = 1;
}

def SUB   : Inst<"sub", 0xB0>;