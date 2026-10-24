// Regression for Xenos dual-issued ALU source reads. Build against the
// patched XenosRecomp sources (same includes/libraries as XenosRecomp).
#include "pch.h"
#include "shader_recompiler.h"
#include <iostream>

int main() {
  auto instruction = [] {
    AluInstruction i{};
    i.vectorOpcode = AluVectorOpcode::Mul;
    i.vectorDest = 3;
    i.vectorWriteMask = 15;
    i.src1Select = i.src2Select = i.src3Select = 1;
    i.src1Register = 1; i.src2Register = 2; i.src3Register = 3;
    i.scalarOpcode = AluScalarOpcode::Adds;
    i.scalarDest = 4; i.scalarWriteMask = 1;
    return i;
  };
  auto check = [](bool condition) {
    if (!condition) { std::cerr << "ALU source regression\n"; std::exit(1); }
  };
  auto i = instruction();
  ShaderRecompiler r;
  r.recompile(i);
  // r3 = r1*r2 and r4.x = old_r3.w + old_r3.x co-issue. With
  // r1=2, r2=3, old_r3=7, the scalar result is 14, not 12.
  check(r.out.find("aluScalarSource = r3;") < r.out.find("r3.xyzw ="));
  check(r.out.find("ps = aluScalarSource.w + aluScalarSource.x;") != std::string::npos);
  i.src3Register = 5;
  r.out.clear(); r.recompile(i);
  check(r.out.find("aluScalarSource =") == std::string::npos);
  check(r.out.find("ps = r5.w + r5.x;") != std::string::npos);
  i = instruction(); i.scalarOpcode = AluScalarOpcode::RetainPrev;
  r.out.clear(); r.recompile(i);
  check(r.out.find("aluScalarSource =") == std::string::npos);
  // Constant/GPR operations encode the GPR index in opcode/select/swizzle.
  i = instruction(); i.scalarOpcode = AluScalarOpcode::Mulsc1;
  i.src3Register = 8; i.src3Swizzle = 0; // GPR = 1 | (1 << 1) = 3.
  r.out.clear(); r.recompile(i);
  check(r.out.find("aluScalarSource = r3;") != std::string::npos);
  check(r.out.find("* aluScalarSource.x") != std::string::npos);
  std::cout << "ALU source checks passed\n";
}
