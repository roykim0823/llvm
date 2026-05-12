def Math {
  int   Sum    = !add(1, 2, 3, 4);        // 10
  int   Diff   = !sub(10, 3);             // 7
  int   Prod   = !mul(2, 3, 4);           // 24
  int   Quot   = !div(20, 6);             // 3   (signed integer division)
  int   Shl    = !shl(1, 4);              // 16
  bit   And    = !and(1, 1, 0);           // 0
  bit   Or     = !or(0, 0, 1);            // 1
  bit   Not    = !not(0);                 // 1
  bit   Eq     = !eq(3, 3);               // 1
  bit   Lt     = !lt(2, 5);               // 1
  int   Lg2    = !logtwo(16);             // 4
}

def Strs {
  string Hi    = !strconcat("Hel", "lo");        // "Hello"
  int    Len   = !size("Hello");                 // 5
  int    Pos   = !find("Hello world", "world");  // 6
  string Sub   = !substr("Hello world", 6, 5);   // "world"
  string Up    = !toupper("abc");                // "ABC"
  string Down  = !tolower("ABC");                // "abc"
  string Rep   = !repr([1, 2, 3]);               // "[1, 2, 3]" (debug only)
}

def Ls {
  list<int> A     = !listconcat([1,2], [3,4]);     // [1,2,3,4]
  list<int> Spl   = !listsplat(7, 3);              // [7,7,7]
  list<int> Rng   = !range(0, 5);                  // [0,1,2,3,4]
  list<int> Rng2  = !range(10, 0, -2);             // [10,8,6,4,2]
  int       Sz    = !size([10, 20, 30]);           // 3
  int       Hd    = !head([10, 20, 30]);           // 10
  list<int> Tl    = !tail([10, 20, 30]);           // [20, 30]
  list<int> Flat  = !listflatten([[1,2],[3,4]]);   // [1,2,3,4]
  string    Join  = !interleave([1,2,3], ", ");    // "1, 2, 3"
}

class Sign<int n> {
  // !if(test, then, else)
  string S1 = !if(!lt(n, 0), "neg", "non-neg");

  // !cond(c1: v1, c2: v2, ..., true: default)   -- evaluated in order
  string S2 = !cond(!lt(n, 0): "negative",
                    !eq(n, 0): "zero",
                    true     : "positive");
}

def A : Sign<-5>;   // S1="neg",     S2="negative"
def B : Sign<0>;    // S1="non-neg", S2="zero"
def C : Sign<7>;    // S1="non-neg", S2="positive"

def Comp {
  list<int> Doubled = !foreach(x, [1,2,3,4], !mul(x, 2));   // [2,4,6,8]
  list<int> Evens   = !filter(x, [1,2,3,4,5,6], !eq(!and(x,1), 0));  // [2,4,6]
  int       Total   = !foldl(0, [1,2,3,4,5], acc, x, !add(acc, x)); // 15
}

class Marker;
def  M : Marker;

def Casting {
  string  AsStr = !cast<string>(M);           // "M"      (record -> name)
  int     I     = !cast<int>(0b1010);         // 10       (bits -> int)
  // Looking up a record by name:
  Marker  Ref   = !cast<Marker>("M");         // -> M
}

class Animal;
def  Dog : Animal;

def Checks {
  bit IsAn   = !isa<Animal>(Dog);           // 1
  bit Exists = !exists<Animal>("Dog");      // 1
  bit Init   = !initialized(?);             // 0
}
