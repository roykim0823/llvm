func.func @loop_add() -> (index) {
  %init = index.constant 0
  %lb = index.constant 0
  %ub = index.constant 10
  %step = index.constant 1

  %sum = scf.for %iv = %lb to %ub step %step iter_args(%acc = %init) -> (index) {
    %sum_next = arith.addi %acc, %iv : index
    scf.yield %sum_next : index
  }
  return %sum : index
}

func.func @main() -> i32 {
  %out = call @loop_add() : () -> index
  %out_i32 = arith.index_cast %out : index to i32
  return %out_i32 : i32
}

// In C this is equivalent to:
//int loop_add(int lb, int ub, int step) {
//  int sum_0 = 0;
//  int sum = sum_0;
//  for (int iv = lb; iv < ub; iv += step) {
//    int sum_next = sum + iv;
//    sum = sum_next;
//  }
//  return sum;
//}

//int main() {
//  int out = loop_add(0, 10, 1);
//  return out;
//}