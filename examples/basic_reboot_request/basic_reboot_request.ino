#include <ESPRebootManager.h>
#include <cstdio>

ESPRebootManager rebootManager;

void setup() {
	Serial.begin(115200);

	ESPRebootManagerConfig config;
	config.taskName = "reboot-manager";
	config.taskStackSizeBytes = 6 * 1024;
	config.taskPriority = 1;
	config.taskCoreId = tskNO_AFFINITY;
	config.callbackTimeoutMs = 1000;
	rebootManager.init(config);

	rebootManager.onRebootRequest([](const RebootRequestContext &ctx) {
		RebootVote vote;

		if (ctx.reason[0] == '\0') {
			vote.allow = false;
			std::snprintf(vote.detail, sizeof(vote.detail), "missing reboot reason");
			return vote;
		}

		// Return false from any callback to block reboot.
		vote.allow = true;
		return vote;
	});

	rebootManager.onEvaluation([](const RebootEvaluation &evaluation) {
		if (evaluation.accepted) {
			Serial.printf(
			    "[reboot] accepted id=%lu reason=%s delay=%lu\n",
			    static_cast<unsigned long>(evaluation.requestId),
			    evaluation.reason,
			    static_cast<unsigned long>(evaluation.delayMs)
			);
			return;
		}

		Serial.printf(
		    "[reboot] rejected id=%lu code=%u blocker=%s detail=%s\n",
		    static_cast<unsigned long>(evaluation.requestId),
		    static_cast<unsigned>(evaluation.code),
		    evaluation.blockerName,
		    evaluation.detail
		);
	});

	RebootSubmitResult result = rebootManager.requestReboot("ExampleReason", 1500);
	Serial.printf(
	    "submit status=%u requestId=%lu\n",
	    static_cast<unsigned>(result.status),
	    static_cast<unsigned long>(result.requestId)
	);
}

void loop() {
	delay(250);
	if (rebootManager.isRebootRequested()) {
		Serial.printf("status=%u\n", static_cast<unsigned>(rebootManager.rebootStatus()));
	}
}
