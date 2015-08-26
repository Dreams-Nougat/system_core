/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "action.h"

#include <errno.h>

#include <base/strings.h>
#include <base/stringprintf.h>

#include "builtins.h"
#include "error.h"
#include "init_parser.h"
#include "log.h"
#include "property_service.h"
#include "util.h"

using android::base::StringPrintf;

Command::Command(int (*f)(const std::vector<std::string>& args),
                 const std::vector<std::string>& args,
                 const std::string& filename,
                 int line)
    : func_(f), args_(args), filename_(filename), line_(line)
{
}

int Command::InvokeFunc() const
{
    std::vector<std::string> expanded_args;
    expanded_args.resize(args_.size());
    expanded_args[0] = args_[0];
    for (std::size_t i = 1; i < args_.size(); ++i) {
        if (expand_props(args_[i], &expanded_args[i]) == -1) {
            ERROR("%s: cannot expand '%s'\n", args_[0].c_str(), args_[i].c_str());
            return -EINVAL;
        }
    }

    return func_(expanded_args);
}

std::string Command::BuildCommandString() const
{
    return android::base::Join(args_, ' ');
}

std::string Command::BuildSourceString() const
{
    if (!filename_.empty()) {
        return StringPrintf(" (%s:%d)", filename_.c_str(), line_);
    } else {
        return std::string();
    }
}

Action::Action(bool oneshot)
    : oneshot_(oneshot) {
}

bool Action::AddCommand(const std::vector<std::string>& args,
                        const std::string& filename, int line, std::string* err)
{
    if (!builtin_keyword_map.count(args[0])) {
        *err = StringPrintf("invalid command '%s'\n", args[0].c_str());
        return false;
    }

    auto command_info = builtin_keyword_map.at(args[0]);

    auto n = std::get<size_t>(command_info);
    if (args.size() < n + 1) {
        *err = StringPrintf("%s requires %zu %s\n",
                            args[0].c_str(), n,
                            n > 2 ? "arguments" : "argument");
        return false;
    }

    auto function = std::get<BuiltinFunction>(command_info);
    AddCommand(function, args, filename, line);
    return true;
}

void Action::AddCommand(int (*f)(const std::vector<std::string>& args),
                        const std::vector<std::string>& args,
                        const std::string& filename, int line)
{
    commands_.emplace_back(f, args, filename, line);
}

std::size_t Action::NumCommands() const
{
    return commands_.size();
}

void Action::ExecuteOneCommand(std::size_t command) const
{
    ExecuteCommand(commands_[command]);
}

void Action::ExecuteAllCommands() const
{
    for (const auto& c : commands_) {
        ExecuteCommand(c);
    }
}

void Action::ExecuteCommand(const Command& command) const
{
    Timer t;
    int result = command.InvokeFunc();

    if (klog_get_level() >= KLOG_INFO_LEVEL) {
        std::string trigger_name = BuildTriggersString();
        std::string cmd_str = command.BuildCommandString();
        std::string source = command.BuildSourceString();

        INFO("Command '%s' action=%s%s returned %d took %.2fs\n",
             cmd_str.c_str(), trigger_name.c_str(), source.c_str(),
             result, t.duration());
    }
}

bool Action::ParsePropertyTrigger(const std::string& trigger, std::string* err)
{
    const static std::string prop_str("property:");
    std::string prop_name(trigger.substr(prop_str.length()));
    size_t equal_pos = prop_name.find('=');
    if (equal_pos == std::string::npos) {
        *err = "property trigger found without matching '='";
        return false;
    }

    std::string prop_value(prop_name.substr(equal_pos + 1));
    prop_name.erase(equal_pos);

    auto res = property_triggers_.emplace(prop_name, prop_value);
    if (res.second == false) {
        *err = "multiple property triggers found for same property";
        return false;
    }
    return true;
}

bool Action::InitTriggers(const std::vector<std::string>& args, std::string* err)
{
    const static std::string prop_str("property:");
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i % 2) {
            if (args[i] != "&&") {
                *err = "&& is the only symbol allowed to concatenate actions";
                return false;
            } else {
                continue;
            }
        }

        if (!args[i].compare(0, prop_str.length(), prop_str)) {
            if (!ParsePropertyTrigger(args[i], err)) {
                return false;
            }
        } else {
            if (!event_trigger_.empty()) {
                *err = "multiple event triggers are not allowed";
                return false;
            }

            event_trigger_ = args[i];
        }
    }

    return true;
}

bool Action::InitSingleTrigger(const std::string& trigger)
{
    std::vector<std::string> name_vector{trigger};
    std::string err;
    return InitTriggers(name_vector, &err);
}

// This function checks that all property triggers are satisfied, i.e.
// for each (name, value) in property_triggers_, check that the current
// value of the property 'name' == value
//
// It takes an optional (name, value) pair, which if provided must
// be present in property_triggers_; it skips the check of the current
// property value for this pair
bool Action::CheckPropertyTriggers(const std::string& name,
                                   const std::string& value) const
{
    if (property_triggers_.empty()) {
        return true;
    }

    bool found = name.empty();
    for (const auto& t : property_triggers_) {
        const auto& trigger_name = t.first;
        const auto& trigger_value = t.second;
        if (trigger_name == name) {
            if (trigger_value != "*" && trigger_value != value) {
                return false;
            } else {
                found = true;
            }
        } else {
            std::string prop_val = property_get(trigger_name.c_str());
            if (prop_val.empty() || (trigger_value != "*" &&
                                     trigger_value != prop_val)) {
                return false;
            }
        }
    }
    return found;
}

bool Action::CheckEventTrigger(const std::string& trigger) const
{
    return !event_trigger_.empty() &&
        trigger == event_trigger_ &&
        CheckPropertyTriggers();
}

bool Action::CheckPropertyTrigger(const std::string& name,
                                  const std::string& value) const
{
    return event_trigger_.empty() && CheckPropertyTriggers(name, value);
}

bool Action::TriggersEqual(const Action& other) const
{
    return property_triggers_ == other.property_triggers_ &&
        event_trigger_ == other.event_trigger_;
}

std::string Action::BuildTriggersString() const
{
    std::string result;

    for (const auto& t : property_triggers_) {
        result += t.first;
        result += '=';
        result += t.second;
        result += ' ';
    }
    if (!event_trigger_.empty()) {
        result += event_trigger_;
        result += ' ';
    }
    result.pop_back();
    return result;
}

void Action::DumpState() const
{
    std::string trigger_name = BuildTriggersString();
    INFO("on %s\n", trigger_name.c_str());

    for (const auto& c : commands_) {
        std::string cmd_str = c.BuildCommandString();
        INFO(" %s\n", cmd_str.c_str());
    }
    INFO("\n");
}

class EventTrigger : public Trigger {
public:
    EventTrigger(const std::string& trigger) : trigger_(trigger) {
    }
    bool CheckTriggers(const Action& action) const override {
        return action.CheckEventTrigger(trigger_);
    }
private:
    const std::string trigger_;
};

class PropertyTrigger : public Trigger {
public:
    PropertyTrigger(const std::string& name, const std::string& value)
        : name_(name), value_(value) {
    }
    bool CheckTriggers(const Action& action) const override {
        return action.CheckPropertyTrigger(name_, value_);
    }
private:
    const std::string name_;
    const std::string value_;
};

class BuiltinTrigger : public Trigger {
public:
    BuiltinTrigger(std::shared_ptr<Action> action) : action_(action) {
    }
    bool CheckTriggers(const Action& action) const override {
        return action_->TriggersEqual(action);
    }
private:
    const std::shared_ptr<Action> action_;
};

ActionManager::ActionManager() : current_command_(0)
{
}

ActionManager& ActionManager::GetInstance() {
    static ActionManager instance;
    return instance;
}

void ActionManager::QueueEventTrigger(const std::string& trigger)
{
    trigger_queue_.push(std::make_unique<EventTrigger>(trigger));
}

void ActionManager::QueuePropertyTrigger(const std::string& name,
                                         const std::string& value)
{
    trigger_queue_.push(std::make_unique<PropertyTrigger>(name, value));
}

void ActionManager::QueueAllPropertyTriggers()
{
    QueuePropertyTrigger("", "");
}

void ActionManager::QueueBuiltinAction(int (*func)(const std::vector<std::string>& args),
                                       const std::string& name)
{
    auto act = std::make_shared<Action>(true);
    std::vector<std::string> name_vector{name};

    if (!act->InitSingleTrigger(name)) {
        return;
    }

    act->AddCommand(func, name_vector);

    actions_.emplace_back(act);
    trigger_queue_.push(std::make_unique<BuiltinTrigger>(act));
}

void ActionManager::ExecuteOneCommand() {
    // Loop through the trigger queue until we have an action to execute
    while (current_executing_actions_.empty() && !trigger_queue_.empty()) {
        std::copy_if(actions_.begin(), actions_.end(),
                     std::back_inserter(current_executing_actions_),
                     [this] (std::shared_ptr<Action>& act) {
                         return trigger_queue_.front()->CheckTriggers(*act);
                     });
        trigger_queue_.pop();
    }

    if (current_executing_actions_.empty()) {
        return;
    }

    auto action = current_executing_actions_.back();

    if (current_command_ == 0) {
        std::string trigger_name = action->BuildTriggersString();
        INFO("processing action (%s)\n", trigger_name.c_str());
    }

    action->ExecuteOneCommand(current_command_);

    // If this was the last command in the current action, then remove
    // the action from the executing list
    // If this action was oneshot, then also remove it from actions_
    ++current_command_;
    if (current_command_ == action->NumCommands()) {
        if (action->oneshot()) {
            actions_.erase(std::remove(actions_.begin(), actions_.end(), action));
        }
        current_command_ = 0;
        current_executing_actions_.pop_back();
    }
}

bool ActionManager::HasMoreCommands() const
{
    return !current_executing_actions_.empty() || !trigger_queue_.empty();
}

void ActionManager::DumpState() const
{
    for (const auto& a : actions_) {
        a->DumpState();
    }
    INFO("\n");
}

class ActionParser : public SectionParser {
public:
    ActionParser(std::vector<std::shared_ptr<Action>>* actions)
        : actions_(actions), action_(nullptr), is_new_(false) {
    }
    bool ParseSection(const std::vector<std::string>& args,
                      std::string* err) override;
    bool ParseLineSection(const std::vector<std::string>& args,
                          const std::string& filename, int line,
                          std::string* err) const override;
    void EndSection() override;
private:
    std::vector<std::shared_ptr<Action>>* actions_;
    std::shared_ptr<Action> action_;
    bool is_new_;
};

bool ActionParser::ParseSection(const std::vector<std::string>& args,
                                std::string* err) {
    std::vector<std::string> triggers(args.begin() + 1, args.end());
    if (triggers.size() < 1) {
        *err = "actions must have a trigger\n";
        return false;
    }

    auto action = std::make_shared<Action>(false);
    if (!action->InitTriggers(triggers, err)) {
        return false;
    }

    auto old_act_it =
        std::find_if(actions_->begin(), actions_->end(),
                     [&action] (std::shared_ptr<Action>& a) {
                         return action->TriggersEqual(*a);
                     });

    if (old_act_it != actions_->end()) {
        action_ = *old_act_it;
        is_new_ = false;
        return true;
    }

    action_ = action;
    is_new_ = true;
    return true;
}

bool ActionParser::ParseLineSection(const std::vector<std::string>& args,
                                    const std::string& filename, int line,
                                    std::string* err) const {
    return action_->AddCommand(args, filename, line, err);
}

void ActionParser::EndSection() {
    if (is_new_ && action_->NumCommands() > 0) {
        actions_->emplace_back(action_);
    }
    is_new_ = false;
    action_.reset();
}

std::unique_ptr<SectionParser> ActionManager::GetSectionParser() {
    return std::make_unique<ActionParser>(&actions_);
}
