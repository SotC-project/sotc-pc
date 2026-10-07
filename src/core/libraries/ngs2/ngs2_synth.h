// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_impl.h"

namespace Libraries::Ngs2::Synth {

s32 RackCreate(u32 rack_id, const OrbisNgs2RackOption* option, OrbisNgs2Handle* out_handle);
s32 RackDestroy(OrbisNgs2Handle rack_handle);
s32 RackGetVoiceHandle(OrbisNgs2Handle rack_handle, u32 index, OrbisNgs2Handle* out_handle);
s32 VoiceControl(OrbisNgs2Handle voice_handle, const OrbisNgs2VoiceParamHeader* params);
s32 VoiceGetState(OrbisNgs2Handle voice_handle, OrbisNgs2VoiceState* out_state, size_t size);
s32 VoiceGetStateFlags(OrbisNgs2Handle voice_handle, u32* out_flags);
s32 Render(const OrbisNgs2RenderBufferInfo* buffers, u32 num_buffers);

} // namespace Libraries::Ngs2::Synth
