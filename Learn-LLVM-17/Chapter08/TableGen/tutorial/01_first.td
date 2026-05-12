def Apple {
  string Color = "red";
  int    Weight = 150;        // grams
  bit    Edible = 1;
  string Origin = ?;          // uninitialized

  list<int> SeedCounts = [2, 3, 5];
  bits<4> RipenessScale = 0b1010; // 10 on a scale of 0-15
}