class Inst<string mnemonic, int opcode> {
  string Mnemonic = mnemonic;
  int Opcode = opcode;
}

multiclass InstWithImm<string mnemonic, int opcode> {
  def "": Inst<mnemonic, opcode>;
  def I : Inst<!strconcat(mnemonic, "i"), !add(opcode, 1)>;
}

defm ADD : InstWithImm<"add", 0xA0>;