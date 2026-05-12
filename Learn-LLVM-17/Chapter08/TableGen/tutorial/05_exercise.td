// =============================================================================
// Lesson 5 — "Try it yourself" solutions
// =============================================================================

// -----------------------------------------------------------------------------
// 1) Top-level `let ... in { ... }` to mark a group of instruction records as
//    calls. Every record in the block picks up `isCall = 1`.
// -----------------------------------------------------------------------------
class Instr {
  bits<8> Opcode = 0;
  bit     isCall = 0;     // default — overridden by the outer `let` below
}

let isCall = 1 in {
  def CALL  : Instr { let Opcode = 0xF0; }
  def CALLR : Instr { let Opcode = 0xF1; }    // call via register
  def BL    : Instr { let Opcode = 0xF2; }    // branch-and-link
}

def RET : Instr { let Opcode = 0xF3; }        // not a call — outside the block

// -----------------------------------------------------------------------------
// 2) Grow a `list<string> Predicates` across an inheritance chain.
//
//    TableGen has no `let append` / `let prepend` operator. The idiomatic way
//    to extend a list across a chain is to thread the extras through class
//    template parameters and `!listconcat` them with the inherited value.
//
//    Each "level" of the chain takes its parent's Predicates, appends its own,
//    and exposes the combined list as the new Predicates.
// -----------------------------------------------------------------------------
class HasPreds<list<string> preds = []> {
  list<string> Predicates = preds;
}

// Each level forwards `Predicates` plus its own additions to the next level.
class L1<list<string> extra = []> : HasPreds<!listconcat(["HasV1"], extra)>;
class L2<list<string> extra = []> : L1<!listconcat(["HasV2"], extra)>;
class L3<list<string> extra = []> : L2<!listconcat(["HasV3"], extra)>;

def Plain     : L3<>;                   // -> ["HasV1", "HasV2", "HasV3"]
def WithFP    : L3<["HasFP"]>;          // -> ["HasV1", "HasV2", "HasV3", "HasFP"]
def WithFPSIMD: L3<["HasFP","HasSIMD"]>;// -> ["HasV1", "HasV2", "HasV3", "HasFP", "HasSIMD"]
