#include "includes.hpp"

#include <limits>

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <tulip/TulipHook.hpp>

constexpr double SMALLEST_FLOAT = std::numeric_limits<float>::min();

// EMA smoothing constants for physics bypass lag detection
constexpr double EMA_ALPHA = 0.05;        // Weight for new samples
constexpr double EMA_MAX_RATIO = 10.0;    // Max delta vs interval ratio
constexpr double LAG_THRESHOLD = 0.0005;  // 0.5ms sustained lag threshold
constexpr double STEP_EPSILON = 0.0001;   // Epsilon for step calculation

constexpr InputEvent EMPTY_INPUT = InputEvent{
	.time = 0,
	.inputType = PlayerButton::Jump,
	.inputState = false,
	.isPlayer1 = false,
};

constexpr Step EMPTY_STEP = Step{
	.input = EMPTY_INPUT,
	.deltaFactor = 1.0,
	.endStep = true,
};

LockFreeQueue<InputEvent, 1024> inputQueue;
std::deque<InputEvent> inputQueueCopy;
StepQueue stepQueue;

std::atomic<bool> softToggle{ false };

InputEvent nextInput = EMPTY_INPUT;

TimestampType lastFrameTime = 0;
TimestampType currentFrameTime = 0;

bool firstFrame = true;
bool skipUpdate = true;
bool enableInput = false;
bool linuxNative = false;
bool lateCutoff = false;

std::array<std::unordered_set<size_t>, 6> inputBinds;
std::unordered_set<uint16_t> heldInputs;

std::mutex keybindsLock;

std::atomic<bool> enableRightClick{ false };
bool threadPriority = true;

double averageDelta = 0.0;

bool physicsBypass = false;
bool legacyBypass = false;
bool safeMode = false;
bool clickOnSteps = false;
bool mouseFix = false;

/*
Optimized step queue builder with physics-accurate collision handling
*/
void buildStepQueue(int stepCount) {
	PlayLayer* playLayer = PlayLayer::get();
	nextInput = EMPTY_INPUT;
	stepQueue.clear();

	if (lateCutoff) {
#ifdef GEODE_IS_WINDOWS
		if (linuxNative) {
			linuxCheckInputs(); // Process inputs BEFORE taking timestamp
		}
#endif
		currentFrameTime = getCurrentTimestamp();

		// Pop all inputs from lock-free queue
		inputQueueCopy.clear();
		InputEvent ev;
		while (inputQueue.pop(ev)) {
			inputQueueCopy.push_back(ev);
		}
	}
	else {
#ifdef GEODE_IS_WINDOWS
		if (linuxNative) linuxCheckInputs();
#endif

		// Only copy inputs that happened before frame start
		inputQueueCopy.clear();
		InputEvent ev;
		while (inputQueue.pop(ev)) {
			if (ev.time <= currentFrameTime) {
				inputQueueCopy.push_back(ev);
			}
			else {
				// Put it back for next frame
				inputQueue.push(ev);
				break;
			}
		}
	}

	skipUpdate = false;
	if (firstFrame) {
		skipUpdate = true;
		firstFrame = false;
		lastFrameTime = currentFrameTime;
		if (!lateCutoff) inputQueueCopy.clear();
		return;
	}

	const TimestampType deltaTime = currentFrameTime - lastFrameTime;
	const TimestampType stepDelta = (deltaTime / stepCount) + 1;

	// Early exit if no inputs - just create end steps
	if (inputQueueCopy.empty()) {
		for (int i = 0; i < stepCount; i++) {
			stepQueue.push(Step{ EMPTY_INPUT, 1.0, true });
		}
		lastFrameTime = currentFrameTime;
		return;
	}

	// Build step queue with precise input timing
	for (int i = 0; i < stepCount; i++) {
		double elapsedTime = 0.0;

		while (!inputQueueCopy.empty()) {
			const InputEvent& front = inputQueueCopy.front();

			if (front.time - lastFrameTime < stepDelta * (i + 1)) {
				const double inputTime = static_cast<double>((front.time - lastFrameTime) % stepDelta) / stepDelta;
				const double deltaFactor = std::clamp(inputTime - elapsedTime, SMALLEST_FLOAT, 1.0);

				if (!stepQueue.push(Step{ front, deltaFactor, false })) {
					log::error("Step queue overflow! This should never happen.");
					break;
				}

				inputQueueCopy.pop_front();
				elapsedTime = inputTime;
			}
			else break;
		}

		const double remainingTime = std::max(SMALLEST_FLOAT, 1.0 - elapsedTime);
		stepQueue.push(Step{ EMPTY_INPUT, remainingTime, true });
	}

	lastFrameTime = currentFrameTime;
}

Step popStepQueue() {
	if (stepQueue.empty()) return EMPTY_STEP;

	Step front = stepQueue.pop();

	if (nextInput.time != 0) {
		PlayLayer* playLayer = PlayLayer::get();

		enableInput = true;
		playLayer->handleButton(nextInput.inputState, (int)nextInput.inputType, nextInput.isPlayer1);
		enableInput = false;
	}

	nextInput = front.input;
	return front;
}

#ifdef GEODE_IS_WINDOWS
#include <geode.custom-keybinds/include/Keybinds.hpp>

void updateKeybinds() {
	std::array<std::unordered_set<size_t>, 6> binds;
	std::vector<geode::Ref<keybinds::Bind>> v;

	enableRightClick.store(Mod::get()->getSettingValue<bool>("right-click"), std::memory_order_relaxed);

	v = keybinds::BindManager::get()->getBindsFor("robtop.geometry-dash/jump-p1");
	for (const auto& bind : v) binds[p1Jump].emplace(bind->getHash());

	v = keybinds::BindManager::get()->getBindsFor("robtop.geometry-dash/move-left-p1");
	for (const auto& bind : v) binds[p1Left].emplace(bind->getHash());

	v = keybinds::BindManager::get()->getBindsFor("robtop.geometry-dash/move-right-p1");
	for (const auto& bind : v) binds[p1Right].emplace(bind->getHash());

	v = keybinds::BindManager::get()->getBindsFor("robtop.geometry-dash/jump-p2");
	for (const auto& bind : v) binds[p2Jump].emplace(bind->getHash());

	v = keybinds::BindManager::get()->getBindsFor("robtop.geometry-dash/move-left-p2");
	for (const auto& bind : v) binds[p2Left].emplace(bind->getHash());

	v = keybinds::BindManager::get()->getBindsFor("robtop.geometry-dash/move-right-p2");
	for (const auto& bind : v) binds[p2Right].emplace(bind->getHash());

	{
		std::lock_guard lock(keybindsLock);
		inputBinds = binds;
	}
}
#endif

void decomp_resetCollisionLog(PlayerObject* p) {
	p->m_collisionLogTop->removeAllObjects();
	p->m_collisionLogBottom->removeAllObjects();
	p->m_collisionLogLeft->removeAllObjects();
	p->m_collisionLogRight->removeAllObjects();
	p->m_lastCollisionLeft = -1;
	p->m_lastCollisionRight = -1;
	p->m_lastCollisionBottom = -1;
	p->m_lastCollisionTop = -1;
}

int calculateStepCount(float delta, float timewarp, bool forceVanilla) {
	// Vanilla 2.2 formula
	if (!physicsBypass || forceVanilla) {
		return static_cast<int>(std::round(std::max(1.0, ((delta * 60.0) / std::min(1.0f, timewarp)) * 4.0)));
	}

	// Legacy 2.1 physics bypass
	if (legacyBypass) {
		return static_cast<int>(std::round(std::max(4.0, delta * 240.0) / std::min(1.0f, timewarp)));
	}

	// Modern 2.2 physics bypass with lag compensation
	const double animationInterval = CCDirector::sharedDirector()->getAnimationInterval();

	// Exponential moving average with saturation protection
	averageDelta = (EMA_ALPHA * delta) + ((1.0 - EMA_ALPHA) * averageDelta);
	averageDelta = std::min(averageDelta, animationInterval * EMA_MAX_RATIO);

	const bool laggingOneFrame = animationInterval < delta - (1.0 / 240.0);
	const bool laggingSustained = averageDelta - animationInterval > LAG_THRESHOLD;

	// No step variance when running smoothly
	if (!laggingOneFrame && !laggingSustained) {
		return static_cast<int>(std::round(std::ceil((animationInterval * 240.0) - STEP_EPSILON) / std::min(1.0f, timewarp)));
	}
	// Sustained low fps
	else if (!laggingOneFrame) {
		return static_cast<int>(std::round(std::ceil(averageDelta * 240.0) / std::min(1.0f, timewarp)));
	}
	// Single frame spike - catch up
	else {
		return static_cast<int>(std::round(std::ceil(delta * 240.0) / std::min(1.0f, timewarp)));
	}
}

class $modify(PlayLayer) {
#ifdef GEODE_IS_WINDOWS
	bool init(GJGameLevel * level, bool useReplay, bool dontCreateObjects) {
		updateKeybinds();
		averageDelta = 0.0; // Reset per-level state
		return PlayLayer::init(level, useReplay, dontCreateObjects);
	}
#endif

	void levelComplete() {
		const bool testMode = this->m_isTestMode;
		if (safeMode && !softToggle.load(std::memory_order_relaxed)) {
			this->m_isTestMode = true;
		}

		PlayLayer::levelComplete();
		this->m_isTestMode = testMode;
	}

	void showNewBest(bool p0, int p1, int p2, bool p3, bool p4, bool p5) {
		if (!safeMode || softToggle.load(std::memory_order_relaxed)) {
			PlayLayer::showNewBest(p0, p1, p2, p3, p4, p5);
		}
	}
};

void onFrameStart() {
	PlayLayer* playLayer = PlayLayer::get();
	CCNode* par;

	if (!lateCutoff) {
		currentFrameTime = getCurrentTimestamp();
	}

	const bool shouldDisable = softToggle.load(std::memory_order_relaxed)
#ifdef GEODE_IS_WINDOWS
		|| !GetFocus()
#endif
		|| !playLayer
		|| !(par = playLayer->getParent())
		|| (par->getChildByType<PauseLayer>(0))
		|| (playLayer->getChildByType<EndLevelLayer>(0));

	if (shouldDisable) {
		firstFrame = true;
		skipUpdate = true;
		enableInput = true;
		inputQueueCopy.clear();

		if (!linuxNative) {
			inputQueue.clear();
		}
	}

#ifdef GEODE_IS_WINDOWS
	if (mouseFix && !skipUpdate) {
		MSG msg;
		// Remove all mouse movements except the last one
		while (PeekMessage(&msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE)) {
			if (msg.message == WM_MOUSEMOVE || msg.message == WM_NCMOUSEMOVE) {
				PeekMessage(&msg, NULL, msg.message, msg.message, PM_REMOVE);
			}
			else break;
		}
	}
#endif
}

#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCEGLView.hpp>
class $modify(CCEGLView) {
	void pollEvents() {
		onFrameStart();
		CCEGLView::pollEvents();
	}
};
#else
#include <Geode/modify/CCScheduler.hpp>
class $modify(CCScheduler) {
	void update(float dt) {
		onFrameStart();
		CCScheduler::update(dt);
	}
};
#endif

int stepCount = 0;

class $modify(GJBaseGameLayer) {
	static void onModify(auto& self) {
		(void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
		(void)self.setHookPriority("GJBaseGameLayer::getModifiedDelta", Priority::VeryEarly);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (enableInput) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}
	}

	float calculateSteps(float modifiedDelta) {
		PlayLayer* pl = PlayLayer::get();
		if (pl) {
			const float timewarp = pl->m_gameState.m_timeWarp;
			if (physicsBypass && (!firstFrame || softToggle.load(std::memory_order_relaxed))) {
				modifiedDelta = CCDirector::sharedDirector()->getActualDeltaTime() * timewarp;
			}

			stepCount = calculateStepCount(modifiedDelta, timewarp, false);

			if (pl->m_playerDied || GameManager::sharedState()->getEditorLayer() || softToggle.load(std::memory_order_relaxed)) {
				enableInput = true;
				skipUpdate = true;
				firstFrame = true;
			}
			else if (modifiedDelta > 0.0) {
				buildStepQueue(stepCount);
			}
			else {
				skipUpdate = true;
			}
		}
		else if (physicsBypass) {
			stepCount = calculateStepCount(modifiedDelta, this->m_gameState.m_timeWarp, true);
		}

		return modifiedDelta;
	}

	void processCommands(float p0) {
		if (clickOnSteps && !stepQueue.empty()) {
			Step step;
			do {
				step = popStepQueue();
			} while (!stepQueue.empty() && !step.endStep);
		}
		GJBaseGameLayer::processCommands(p0);
	}

	float getModifiedDelta(float delta) {
		return calculateSteps(GJBaseGameLayer::getModifiedDelta(delta));
	}

#ifdef GEODE_IS_MACOS
	void update(float delta) {
		if (this->m_started) {
			const float timewarp = std::max(this->m_gameState.m_timeWarp, 1.0f) / 240.0f;
			calculateSteps(roundf((this->m_extraDelta + (m_resumeTimer <= 0 ? delta : 0.0)) / timewarp) * timewarp);
		}

		GJBaseGameLayer::update(delta);
	}
#endif
};

CCPoint p1Pos = { 0.f, 0.f };
CCPoint p2Pos = { 0.f, 0.f };

float rotationDelta = 0.0f;
float shipRotDelta = 0.0f;
bool inputThisStep = false;
bool p1Split = false;
bool p2Split = false;
bool midStep = false;

class $modify(PlayerObject) {
	/*
	CRITICAL FIX: This function now properly handles collision detection.

	The key insight: vanilla GD calls checkCollisions with the FULL physics step,
	never with substeps. We split the rotation/position updates but NOT the collision
	detection itself to maintain vanilla physics accuracy.
	*/
	void update(float stepDelta) {
		PlayLayer* pl = PlayLayer::get();
		if (!skipUpdate) enableInput = false;

		// P2 or mid-step processing
		if ((pl && this != pl->m_player1) || midStep) {
			if (midStep || !inputThisStep || this != pl->m_player2) {
				PlayerObject::update(stepDelta);
			}
			return;
		}

		inputThisStep = stepQueue.empty() ? false : !stepQueue.steps[stepQueue.read_idx].endStep;
		if (!stepQueue.empty() && !inputThisStep && !clickOnSteps) {
			stepQueue.pop();
		}

		// Skip or non-input frames - use vanilla update
		if (skipUpdate || !pl || !inputThisStep || clickOnSteps) {
			p1Split = false;
			p2Split = false;
			inputThisStep = false;
			PlayerObject::update(stepDelta);
			return;
		}

		PlayerObject* p2 = pl->m_player2;
		const bool isDual = pl->m_gameState.m_isDualMode;
		const bool p1StartedOnGround = this->m_isOnGround;
		const bool p2StartedOnGround = p2->m_isOnGround;

		// Determine if we should split physics for each player
		const bool p1NotBuffering = p1StartedOnGround
			|| this->m_touchingRings->count()
			|| this->m_isDashing
			|| (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);

		const bool p2NotBuffering = p2StartedOnGround
			|| p2->m_touchingRings->count()
			|| p2->m_isDashing
			|| (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing);

		p1Pos = PlayerObject::getPosition();
		p2Pos = p2->getPosition();

		p1Split = p1NotBuffering;
		p2Split = p2NotBuffering && isDual;

		Step step;
		bool firstLoop = true;
		midStep = true;

		// Process substeps until we hit an endStep
		do {
			step = popStepQueue();
			const float substepDelta = stepDelta * step.deltaFactor;
			rotationDelta = substepDelta;

			if (p1Split) {
				// CRITICAL: We call update with substepDelta for position/velocity
				PlayerObject::update(substepDelta);

				if (!step.endStep) {
					// Fix ground state for delayed inputs on moving platforms
					if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) {
						this->m_isOnGround = p1StartedOnGround;
					}

					// PHYSICS FIX: Always use stepDelta for collision detection
					// This matches vanilla behavior exactly
					pl->checkCollisions(this, stepDelta, true);
					PlayerObject::updateRotation(substepDelta);
					decomp_resetCollisionLog(this);
				}
			}
			else if (step.endStep) {
				// Not splitting - use vanilla physics
				PlayerObject::update(stepDelta);
			}

			if (p2Split) {
				p2->update(substepDelta);

				if (!step.endStep) {
					if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) {
						p2->m_isOnGround = p2StartedOnGround;
					}

					pl->checkCollisions(p2, stepDelta, true);
					p2->updateRotation(substepDelta);
					decomp_resetCollisionLog(p2);
				}
			}
			else if (step.endStep) {
				p2->update(stepDelta);
			}

			firstLoop = false;
		} while (!step.endStep);

		midStep = false;
	}

	void updateRotation(float t) {
		PlayLayer* pl = PlayLayer::get();

		if (pl && this == pl->m_player1 && p1Split && !midStep) {
			PlayerObject::updateRotation(rotationDelta);
			this->m_lastPosition = p1Pos;
		}
		else if (pl && this == pl->m_player2 && p2Split && !midStep) {
			PlayerObject::updateRotation(rotationDelta);
			this->m_lastPosition = p2Pos;
		}
		else {
			PlayerObject::updateRotation(t);
		}

		// Fix percent calculation with physics bypass
		if (physicsBypass && pl && !midStep) {
			pl->m_gameState.m_currentProgress = static_cast<int>(pl->m_gameState.m_levelTime * 240.0);
		}
	}

#ifdef GEODE_IS_WINDOWS
	void updateShipRotation(float t) {
		PlayLayer* pl = PlayLayer::get();

		if (pl && (this == pl->m_player1 || this == pl->m_player2) && (physicsBypass || inputThisStep)) {
			shipRotDelta = t;
			// Use 1/1024 to get precise rotation (matched in Slerp2D hook)
			PlayerObject::updateShipRotation(1.0f / 1024.0f);
			shipRotDelta = 0.0f;
		}
		else {
			PlayerObject::updateShipRotation(t);
		}
	}
#endif
};

class $modify(EndLevelLayer) {
	void customSetup() {
		EndLevelLayer::customSetup();

		if (!softToggle.load(std::memory_order_relaxed) || physicsBypass) {
			std::string text;

			if ((softToggle.load(std::memory_order_relaxed) || clickOnSteps) && physicsBypass) {
				text = "PB";
			}
			else if (physicsBypass) {
				text = "CBF+PB";
			}
			else if (!clickOnSteps && !softToggle.load(std::memory_order_relaxed)) {
				text = "CBF";
			}
			else {
				return;
			}

			cocos2d::CCSize size = cocos2d::CCDirector::sharedDirector()->getWinSize();
			CCLabelBMFont* indicator = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");

			indicator->setPosition({ size.width, size.height });
			indicator->setAnchorPoint({ 1.0f, 1.0f });
			indicator->setOpacity(30);
			indicator->setScale(0.2f);

			this->addChild(indicator);
		}
	}
};

class $modify(GJGameLevel) {
	void savePercentage(int percent, bool p1, int clicks, int attempts, bool valid) {
		valid = (
			softToggle.load(std::memory_order_relaxed) && !physicsBypass
			|| clickOnSteps && !physicsBypass
			|| this->m_stars == 0
			);

		if (!safeMode || softToggle.load(std::memory_order_relaxed)) {
			GJGameLevel::savePercentage(percent, p1, clicks, attempts, valid);
		}
	}
};

float Slerp2D(float p0, float p1, float p2) {
	auto orig = reinterpret_cast<float (*)(float, float, float)>(geode::base::get() + 0x71ec0);
	if (shipRotDelta != 0.0f) {
		// Compensate for the 1/1024 scaling in updateShipRotation
		shipRotDelta *= p2 * 1024.0f;
		return orig(p0, p1, shipRotDelta);
	}
	return orig(p0, p1, p2);
}

void togglePhysicsBypass(bool enable) {
#ifdef GEODE_IS_WINDOWS
	void* addr = reinterpret_cast<void*>(geode::base::get() + 0x2322ca);

	static Patch* pbPatch = nullptr;
	if (!pbPatch) {
		geode::ByteVector bytes = { 0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0, 0x44, 0x8b, 0x19 };
		int* stepAddr = &stepCount;
		for (int i = 0; i < 8; i++) {
			bytes[i + 2] = ((char*)&stepAddr)[i];
		}
		log::info("Physics bypass patch: {} at {}", bytes, addr);
		pbPatch = Mod::get()->patch(addr, bytes).unwrap();
	}

	if (enable) {
		(void)pbPatch->enable();
	}
	else {
		(void)pbPatch->disable();
	}

	physicsBypass = enable;
#endif
}

void toggleMod(bool disable) {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_ANDROID64)
	void* addr = reinterpret_cast<void*>(geode::base::get() + GEODE_WINDOWS(0x607230) GEODE_ANDROID64(0x5c00d0));

	static Patch* modPatch = nullptr;
	if (!modPatch) {
		modPatch = Mod::get()->patch(addr, { 0x29, 0x5c, 0x4f, 0x3f }).unwrap();
	}

	if (disable) {
		(void)modPatch->disable();
	}
	else {
		(void)modPatch->enable();
	}
#endif

	softToggle.store(disable, std::memory_order_relaxed);
}

$on_mod(Loaded) {
	Mod::get()->setSavedValue<bool>("is-linux", false);

	toggleMod(Mod::get()->getSettingValue<bool>("soft-toggle"));
	listenForSettingChanges("soft-toggle", toggleMod);

	togglePhysicsBypass(Mod::get()->getSettingValue<bool>("physics-bypass"));
	listenForSettingChanges("physics-bypass", togglePhysicsBypass);

	legacyBypass = Mod::get()->getSettingValue<std::string>("bypass-mode") == "2.1";
	listenForSettingChanges("bypass-mode", +[](std::string mode) {
		legacyBypass = mode == "2.1";
		});

	safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
	listenForSettingChanges("safe-mode", +[](bool enable) {
		safeMode = enable;
		});

	clickOnSteps = Mod::get()->getSettingValue<bool>("click-on-steps");
	listenForSettingChanges("click-on-steps", +[](bool enable) {
		clickOnSteps = enable;
		});

	mouseFix = Mod::get()->getSettingValue<bool>("mouse-fix");
	listenForSettingChanges("mouse-fix", +[](bool enable) {
		mouseFix = enable;
		});

	lateCutoff = Mod::get()->getSettingValue<bool>("late-cutoff");
	listenForSettingChanges("late-cutoff", +[](bool enable) {
		lateCutoff = enable;
		});

	threadPriority = Mod::get()->getSettingValue<bool>("thread-priority");

#ifdef GEODE_IS_WINDOWS
	(void) Mod::get()->hook(
		reinterpret_cast<void*>(geode::base::get() + 0x71ec0),
		Slerp2D,
		"Slerp2D",
		tulip::hook::TulipConvention::Default
	);

	windowsSetup();
#endif
}