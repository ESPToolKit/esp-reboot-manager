#include <Arduino.h>
#include <ESPRebootManager.h>
#include <cstdio>

ESPRebootManager rebootManager;

bool submitted = false;
bool printedFinal = false;

void setup() {
	Serial.begin(115200);
	while (!Serial) {
		delay(10);
	}

	ESPRebootManagerConfig config;
	config.taskName = "reboot-manager";
	config.callbackTimeoutMs = 200;
	config.rebootExecutor = []() {
		Serial.println("[executor] should not run when callback times out");
	};

	if (!rebootManager.init(config)) {
		Serial.println("Failed to init ESPRebootManager");
		return;
	}

	rebootManager.onRebootRequest([](const RebootRequestContext &) {
		RebootVote vote;
		std::snprintf(vote.detail, sizeof(vote.detail), "guard took too long");
		delay(600);
		vote.allow = true;
		return vote;
	});

	rebootManager.onEvaluation([](const RebootEvaluation &evaluation) {
		Serial.printf(
		    "[evaluation] accepted=%u code=%u blocker=%s detail=%s\n",
		    static_cast<unsigned>(evaluation.accepted),
		    static_cast<unsigned>(evaluation.code),
		    evaluation.blockerName,
		    evaluation.detail
		);
	});

	RebootSubmitResult result = rebootManager.requestReboot("simulate-timeout", 0);
	submitted = (result.status == RebootSubmitStatus::Queued);
	Serial.printf("submit status=%u\n", static_cast<unsigned>(result.status));
}

void loop() {
	if (!submitted || printedFinal) {
		delay(200);
		return;
	}

	if (rebootManager.rebootStatus() == RebootRequestStatus::Idle) {
		const RebootEvaluation latest = rebootManager.lastEvaluation();
		Serial.printf(
		    "final accepted=%u code=%u detail=%s\n",
		    static_cast<unsigned>(latest.accepted),
		    static_cast<unsigned>(latest.code),
		    latest.detail
		);
		printedFinal = true;
	}

	delay(200);
}
