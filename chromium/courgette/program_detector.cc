// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "courgette/program_detector.h"

#include "courgette/disassembler.h"
#include "courgette/disassembler_elf_32_arm.h"
#include "courgette/disassembler_elf_32_x86.h"
#include "courgette/disassembler_win32_x64.h"
#include "courgette/disassembler_win32_x86.h"

namespace courgette {

std::unique_ptr<Disassembler> DetectDisassembler(const uint8_t* buffer,
                                                 size_t length) {
  std::unique_ptr<Disassembler> disassembler;

  if (DisassemblerWin32X86::QuickDetect(buffer, length)) {
    disassembler.reset(new DisassemblerWin32X86(buffer, length));
    if (disassembler->ParseHeader())
      return disassembler;
  }
  if (DisassemblerWin32X64::QuickDetect(buffer, length)) {
    disassembler.reset(new DisassemblerWin32X64(buffer, length));
    if (disassembler->ParseHeader())
      return disassembler;
  }
  if (DisassemblerElf32X86::QuickDetect(buffer, length)) {
    disassembler.reset(new DisassemblerElf32X86(buffer, length));
    if (disassembler->ParseHeader())
      return disassembler;
  }
  if (DisassemblerElf32ARM::QuickDetect(buffer, length)) {
    disassembler.reset(new DisassemblerElf32ARM(buffer, length));
    if (disassembler->ParseHeader())
      return disassembler;
  }
  return nullptr;
}

Status DetectExecutableType(const uint8_t* buffer,
                            size_t length,
                            ExecutableType* type,
                            size_t* detected_length) {
  std::unique_ptr<Disassembler> disassembler(
      DetectDisassembler(buffer, length));

  if (!disassembler) {  // We failed to detect anything.
    *type = EXE_UNKNOWN;
    *detected_length = 0;
    return C_INPUT_NOT_RECOGNIZED;
  }

  *type = disassembler->kind();
  *detected_length = disassembler->length();
  return C_OK;
}

}  // namespace courgette
