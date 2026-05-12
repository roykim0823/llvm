class Fruit {
  string Color  = "unknown";
  bit    Edible = 1;
}

def Apple  : Fruit { let Color = "red"; }
def Lemon  : Fruit { let Color = "yellow"; }
def Acorn  : Fruit { let Edible = 0; }      // keeps Color = "unknown"


class HasPrice  { int Price = 0; }
class HasColor  { string Color = "white"; }

def Banana : HasPrice, HasColor {
  let Price = 50;
  let Color = "yellow";
}

class Vehicle {
  int Wheels = 4;
  bit Motorized = 1;
}

def Bicycle : Vehicle {
  let Wheels = 2;
  let Motorized = 0;
}

def Truck   : Vehicle{
  let Wheels = 6;
  let Motorized = 1;
}

def Skateboard : Vehicle{
  let Wheels = 4;
  let Motorized = 0;
}