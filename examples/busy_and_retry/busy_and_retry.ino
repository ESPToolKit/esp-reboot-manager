#include <Arduino.h>
#include <ESPRebootManager.h>

ESPRebootManager rebootManager;

bool firstQueued = false;
bool secondRetried = false;
bool finished = false;

void printSubmitResult(const char *label, const RebootSubmitResult &result) {
	Serial.printf(
	    "%s submit status=%u requestId=%lu\n",
	    label,
	    static_cast<unsigned>(result.status),
	    static_cast<unsigned long>(result.requestId)
	);
}

void setup() {
	Serial.begin(115200);
	while (!Serial) {
		delay(10);
	}

	ESPRebootManagerConfig config;
	config.taskName = "reboot-manager";
	config.callbackTimeoutMs = 1000;
	config.rebootExecutor = []() { Serial.println("[executor] simulated reboot"); };

	if (!rebootManager.init(config)) {
		Serial.println("Failed to init ESPRebootManager");
		return;
	}

	rebootManager.onRebootRequest([](const RebootRequestContext &) { return RebootVote{}; });

	rebootManager.onEvaluation([](const RebootEvaluation &evaluation) {
		Serial.printf(
		    "[evaluation] accepted=%u code=%u id=%lu reason=%s delayMs=%lu\n",
		    static_cast<unsigned>(evaluation.accepted),
		    static_cast<unsigned>(evaluation.code),
		    static_cast<unsigned long>(evaluation.requestId),
		    evaluation.reason,
		    static_cast<unsigned long>(evaluation.delayMs)
		);
	});

	RebootSubmitResult first = rebootManager.requestReboot("firmware-update", 4000);
	printSubmitResult("first", first);
	firstQueued = (first.status == RebootSubmitStatus::Queued);

	// A second request during the first one should return Busy.
	RebootSubmitResult second = rebootManager.requestReboot("wifi-reset", 0);
	printSubmitResult("second-immediate", second);
}

void loop() {
	if (!firstQueued || finished) {
		delay(250);
		return;
	}

	const RebootRequestStatus status = rebootManager.rebootStatus();
	Serial.printf("[status] %u\n", static_cast<unsigned>(status));

	if (!secondRetried && status == RebootRequestStatus::Idle) {
		secondRetried = true;
		RebootSubmitResult retry = rebootManager.requestReboot("wifi-reset", 0);
		printSubmitResult("second-retry", retry);
		finished = (retry.status == RebootSubmitStatus::Queued);
	}

	delay(500);
}
