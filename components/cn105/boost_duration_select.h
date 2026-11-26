#pragma once

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include <functional>
#include <string>

namespace esphome {

    class BoostDurationSelect : public select::Select, public Component {
    public:
        using CallbackFunction = std::function<void(const std::string& value)>;

        void setCallbackFunction(CallbackFunction&& callback) {
            this->callBackFunction = std::move(callback);
        }

    protected:
        void control(const std::string& value) override {
            if (this->callBackFunction) {
                this->callBackFunction(value);
            }
            // select::SelectCall does not publish automatically. Publish here so
            // HA and the web UI reflect the choice even before the next BOOST.
            this->publish_state(value);
        }

    private:
        CallbackFunction callBackFunction;

    };

}  // namespace esphome
