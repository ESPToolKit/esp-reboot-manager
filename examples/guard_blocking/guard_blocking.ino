#include <Arduino.h>
#include <ESPRebootManager.h>
#include <cstdio>

ESPRebootManager rebootManager;

bool maintenanceWindowOpen = false;
bool firstQueued = false;
bool secondQueued = false;

void setup() {
	Serial.begin(115200);
	while (!Serial) {
		delay(10);
	}

	ESPRebootManagerConfig config;
	config.taskName = "reboot-manager";
	config.callbackTimeoutMs = 1000;
	config.rebootExecutor = []() {
		Serial.println("[executor] simulated reboot");
	};

	if (!rebootManager.init(config)) {
		Serial.println("Failed to init ESPRebootManager");
		return;
	}

	rebootManager.onRebootRequest([](const RebootRequestContext &) {
		RebootVote vote;
		if (!maintenanceWindowOpen) {
			vote.allow = false;
			std::snprintf(vote.detail, sizeof(vote.detail), "maintenance window is closed");
		}
		return vote;
	});

	rebootManager.onEvaluation([](const RebootEvaluation &evaluation) {
		if (evaluation.accepted) {
			Serial.printf(
                "[evaluation] accepted id=%lu reason=%s\n",
                static_cast<unsigned long>(evaluation.requestId),
                evaluation.reason
            );
			return;
		}

		Serial.printf(
            "[evaluation] rejected id=%lu code=%u blocker=%s detail=%s\n",
            static_cast<unsigned long>(evaluation.requestId),
            static_cast<unsigned>(evaluation.code),
            evaluation.blockerName,
            evaluation.detail
        );
	});

	RebootSubmitResult first = rebootManager.requestReboot("scheduled-maintenance", 1000);
	firstQueued = (first.status == RebootSubmitStatus::Queued);
	Serial.printf("first submit status=%u\n", static_cast<unsigned>(first.status));
}

void loop() {
	static uint32_t startedAt = millis();

	if (firstQueued && !maintenanceWindowOpen && millis() - startedAt >= 5000) {
		maintenanceWindowOpen = true;
		Serial.println("Maintenance window opened. Retrying reboot request...");
	}

	if (maintenanceWindowOpen && !secondQueued && rebootManager.rebootStatus() == RebootRequestStatus::Idle) {
		RebootSubmitResult second = rebootManager.requestReboot("scheduled-maintenance", 1000);
		secondQueued = (second.status == RebootSubmitStatus::Queued);
		Serial.printf("second submit status=%u\n", static_cast<unsigned>(second.status));
	}

	delay(250);
}
