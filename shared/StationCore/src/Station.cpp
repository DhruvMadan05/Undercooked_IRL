#include "Station.h"

namespace station {

void Station::begin() {
  if (task_) task_->begin();
  display_.begin();
  display_.setOffline(true);
  session_.begin();

  session_.onChange([this](bool connected) { display_.setOffline(!connected); });

  // A server that just (re)started knows nothing about the tag on us.
  session_.onWelcome([this]() {
    if (!reader_.present()) return;
    if (task_) task_->stop();
    display_.clearProgress();
    announceTag();
  });

  oc::on<oc::MsgType::Accept>([this](const uint8_t *mac, const oc::AcceptMsg &msg) {
    session_.heardServer(mac);
    if (state_ != State::Awaiting || !oc::sameTag(msg.tag, tag_)) return;

    bool runnable = task_ && msg.task != oc::TaskKind::None && task_->kind() == msg.task;
    if (msg.task != oc::TaskKind::None && !runnable) {
      Serial.printf("Server wants task %u, this station cannot run it\n", (uint8_t)msg.task);
    }
    if (!runnable) {
      state_ = State::Passive;
      return;
    }

    goal_ = msg.goal ? msg.goal : 1;
    lastReported_ = msg.progress;
    task_->start(msg.goal, msg.progress, msg.param);
    display_.setProgress(msg.progress, goal_);
    state_ = State::Active;
    Serial.printf("Task started: %u/%u\n", msg.progress, msg.goal);
  });

  oc::on<oc::MsgType::Reject>([this](const uint8_t *mac, const oc::RejectMsg &msg) {
    session_.heardServer(mac);
    if (state_ != State::Awaiting || !oc::sameTag(msg.tag, tag_)) return;
    state_ = State::Rejected;
    display_.flash(oc::DisplayMode::Reject);
    Serial.println("Server rejected the tag");
  });

  oc::on<oc::MsgType::SetDisplay>([this](const uint8_t *mac, const oc::SetDisplayMsg &msg) {
    session_.heardServer(mac);
    display_.setMode(msg.mode, msg.level);
  });

  oc::onSendFailed([](const uint8_t *mac, oc::MsgType type) {
    // Hello is expected to fail while the bridge is off; the session retries it.
    if (type == oc::MsgType::Hello) return;
    Serial.printf("Message %u to %02X:%02X:%02X:%02X:%02X:%02X was not delivered\n", (uint8_t)type,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  });
}

oc::TagId Station::tagOf(const tagreader::Uid &uid) const {
  return oc::makeTagId(uid.bytes, uid.size);
}

void Station::announceTag() {
  state_ = State::Awaiting;
  session_.send<oc::MsgType::TagPlaced>({tag_});
  Serial.print("Tag placed: ");
  oc::printTag(tag_);
  Serial.println();
}

void Station::onPlaced(const tagreader::Uid &uid) {
  tag_ = tagOf(uid);
  announceTag();
}

void Station::onRemoved(const tagreader::Uid &uid) {
  oc::TagId gone = tagOf(uid);
  uint16_t progress = (state_ == State::Active && task_) ? task_->progress() : 0;
  if (task_) task_->stop();
  display_.clearProgress();
  state_ = State::Empty;
  session_.send<oc::MsgType::TagRemoved>({gone, progress});
  Serial.print("Tag removed: ");
  oc::printTag(gone);
  Serial.printf(" (progress %u)\n", progress);
}

void Station::reportProgress(uint32_t now) {
  uint16_t progress = task_->progress();
  display_.setProgress(progress, goal_);
  if (progress == lastReported_ || (int32_t)(now - nextProgressAt_) < 0) return;
  lastReported_ = progress;
  nextProgressAt_ = now + kProgressIntervalMs;
  session_.send<oc::MsgType::TaskProgress>({tag_, progress});
}

void Station::update(uint32_t now) {
  oc::poll();
  session_.update(now);

  tagreader::Change change = reader_.poll(now);
  if (change.removed) onRemoved(change.removedUid);
  if (change.placed) onPlaced(change.placedUid);

  // Always run the task, even with no tag, so its inputs (debounce) stay current.
  if (task_) task_->update(now);

  if (state_ == State::Active) {
    if (task_->done()) {
      session_.send<oc::MsgType::TaskDone>({tag_});
      display_.clearProgress();
      display_.flash(oc::DisplayMode::Success);
      state_ = State::Finished;
      Serial.println("Task done, told server");
    } else {
      reportProgress(now);
    }
  }

  display_.update(now);
}

} // namespace station
