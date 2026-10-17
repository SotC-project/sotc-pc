// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Weapon wheel: hold G in play, the game pauses, point at a weapon with the mouse and let go to
// take it in hand. Every weapon of the game is offered, its bonus ones included; one not owned
// yet is given to Wander when chosen.
namespace Sotc::Wheel {

/// Draws the wheel and runs the change of weapon. Call once per frame from the overlay.
void Draw();
/// True while the wheel is up: the mouse then points on it instead of turning the view.
bool IsOpen();

} // namespace Sotc::Wheel
