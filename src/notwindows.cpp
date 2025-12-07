#include "includes.hpp"

TimestampType pendingInputTimestamp = 0;

#include <Geode/modify/GJBaseGameLayer.hpp>
class $modify(GJBaseGameLayer) {
	void queueButton(int button, bool push, bool isPlayer2) {
		if (!softToggle.load() && pendingInputTimestamp) {
			InputEvent ev{
				.time = pendingInputTimestamp,
				.inputType = PlayerButton(button),
				.inputState = push ? State::Press : State::Release,
				.isPlayer1 = !isPlayer2
			};

			if (!inputQueue.push(ev)) {
				// Fallback to slower method if queue is full
				log::warn("Input queue full in queueButton");
			}
		}

		GJBaseGameLayer::queueButton(button, push, isPlayer2);
	}
};