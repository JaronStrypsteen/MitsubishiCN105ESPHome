#include "cn105.h"
#include "heatpumpFunctions.h"
#include "Globals.h"

using namespace esphome;
//#region heatpump_functions fonctions clim

void CN105Climate::functionsArrived() {
    if (this->profile_->allow_operation(cn105::ProfileFeature::AUXILIARY_CONTROLS)) {
        this->profile_->apply_functions();
    }
}

void CN105Climate::send_pending_functions_part2_() {
    if (!this->has_pending_functions_part2_) return;
    this->has_pending_functions_part2_ = false;
    this->cancel_timeout("cn105_func_p2");
    ESP_LOGD(TAG, "sending a setFunctions packet part 2");
    this->writePacket(this->pending_functions_part2_packet_, PACKET_LEN);
    this->isGetFunctions_ = true;
}

bool CN105Climate::setFunctions(heatpumpFunctions const& functions) {
    if (!this->profile_->allow_operation(cn105::ProfileFeature::AUXILIARY_CONTROLS)) {
        return false;
    }
    if (!functions.isValid()) {
        ESP_LOGW(LOG_FUNCTIONS_TAG, "Cannot send functions: function settings are not valid");
        return false;
    }

    uint8_t packet1[PACKET_LEN] = {};
    uint8_t packet2[PACKET_LEN] = {};
    prepareSetPacket(packet1, PACKET_LEN);
    prepareSetPacket(packet2, PACKET_LEN);
    if (!this->profile_->encode_functions(packet1, packet2, functions)) return false;

    packet1[21] = checkSum(packet1, 21);
    packet2[21] = checkSum(packet2, 21);

    ESP_LOGD(TAG, "sending a setFunctions packet part 1");
    if (writePacket(packet1, PACKET_LEN)) {
        std::memcpy(this->pending_functions_part2_packet_, packet2, PACKET_LEN);
        this->has_pending_functions_part2_ = true;
        this->set_timeout("cn105_func_p2", 350, [this]() {
            this->send_pending_functions_part2_();
        });
    } else {
        this->enqueue_pending_write_(packet2, PACKET_LEN, true, PendingWriteKind::GENERAL, 0.0f);
        this->isGetFunctions_ = true;
    }

    return true;
}
