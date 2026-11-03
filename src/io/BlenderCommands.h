// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Blender bridge commands: blender_send and blender_receive (io/BlenderBridge.h).
// Registered by CommandRegistry's constructor.
#pragma once

#include "core/Commands.h"

namespace sf {

void registerBlenderCommands(CommandRegistry& registry);

} // namespace sf
