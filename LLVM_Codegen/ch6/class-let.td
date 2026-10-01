class MyClass<string _alias=""> {
  string alias = _alias;
}

let alias = "let from out" in def A : MyClass<> {}

def B: MyClass<> {
  string alias = "let from body";
}

def C: MyClass<"from arg">;

let alias = "alias from bigger scope" in {
  let alias = "let from out" in 
    def D : MyClass<"from arg"> {
      let alias = "lef from body";
    }

  def E: MyClass<"will be overriden">;
}