#include "service.h"
#include "batch.h"
#include "definitions.h"

#include <string>
#include <utility>

namespace marian {
namespace bergamot {

Service::Service(Ptr<Options> options)
    : requestId_(0), numWorkers_(options->get<int>("cpu-threads")),
      vocabs_(std::move(loadVocabularies(options))),
      text_processor_(vocabs_, options), batcher_(options),
      capacityBytes_(options->get<int>("capacity-bytes")),
      pcqueue_(2 * options->get<int>("cpu-threads")) {

  if (numWorkers_ == 0) {
    // In case workers are 0, a single-translator is created and initialized
    // in the main thread.
    marian::DeviceId deviceId(/*cpuId=*/0, DeviceType::cpu);
    translators_.emplace_back(deviceId, vocabs_, options);
    translators_.back().initialize();
  } else {
    // If workers specified are greater than 0, translators_ are populated with
    // unitialized instances. These are then initialized inside
    // individual threads and set to consume from producer-consumer queue.
    workers_.reserve(numWorkers_);
    translators_.reserve(numWorkers_);
    for (size_t cpuId = 0; cpuId < numWorkers_; cpuId++) {
      marian::DeviceId deviceId(cpuId, DeviceType::cpu);
      translators_.emplace_back(deviceId, vocabs_, options);

      auto &translator = translators_.back();
      workers_.emplace_back([&translator, this] {
        translator.initialize();
        translator.consumeFrom(pcqueue_);
      });
    }
  }
}

std::future<Response> Service::translateWithCopy(std::string input) {
  return translate(std::move(input));
}

std::future<Response> Service::translate(std::string &&input) {
  // Takes in a blob of text. Segments and SentenceRanges are
  // extracted from the input (blob of text) and used to construct a Request
  // along with a promise. promise value is set by the worker completing a
  // request.
  //
  // Batcher, which currently runs on the main thread constructs batches out of
  // a single request (at the moment) and adds them into a Producer-Consumer
  // queue holding a bunch of requestSentences used to construct batches.
  // TODO(jerin): Make asynchronous and compile from multiple requests.
  //
  // returns future corresponding to the promise.
  Ptr<RequestTracker> tracker =
      std::move(translatePart(std::move(input), /*lineNumberBegin=*/0));
  std::future<Response> future = std::move(tracker->future);
  return future;
}

Ptr<RequestTracker> Service::translatePart(std::string &&input,
                                           int lineNumberBegin) {

  Ptr<RequestTracker> tracker = New<RequestTracker>();
  std::promise<Response> responsePromise;
  auto future = responsePromise.get_future();
  tracker->set_future(std::move(future));

  size_t inputBytes = input.size();

  if (inputBytes > capacityBytes_) {
    // Check if input exceeds capacity, reject if this is the case.
    tracker->setStatus(StatusCode::REJECTED_MEMORY);
    Response emptyResponse = Response::EmptyResponse();
    responsePromise.set_value(std::move(emptyResponse));
  } else {

    // Accept request in, and adjust capacity accordingly.
    capacityBytes_ -= input.size();
    LOG(info, "CapacityBytes {}", capacityBytes_.load());

    // A prepareRequest lambda allows this segment to be executed  synchronously
    // or asynchronously, both of which are done below.
    auto prepareRequest = [&]() {
      Segments segments;
      SentenceRanges sourceRanges;
      text_processor_.process(input, segments, sourceRanges);

      Ptr<Request> request =
          New<Request>(requestId_++, lineNumberBegin, /*nice=*/20, vocabs_,
                       std::move(input), std::move(segments),
                       std::move(sourceRanges), std::move(responsePromise));

      // Set tracker to track request. Set tracker on request so tracker is
      // updated when request is complete.
      tracker->track(request);

      auto callback = [tracker, this, inputBytes]() {
        tracker->setStatus(StatusCode::SUCCESS);
        capacityBytes_ += inputBytes;
        LOG(info, "CapacityBytes {}", capacityBytes_.load());
      };

      request->onCompleteRequest(std::move(callback));

      batcher_.addWholeRequest(request);
      tracker->setStatus(StatusCode::QUEUED);
    };

    if (numWorkers_ > 0) {
      // If there are more than 1 workers, we're operating in async multithread
      // setting. The preprocessing and queue calls can also be done in the
      // background.
      auto queueInBackground = [&]() {
        prepareRequest();
        batcher_.produceTo(pcqueue_);
      };
      std::async(std::launch::async, queueInBackground);

    } else {
      // Queue single-threaded
      prepareRequest();
      Batch batch;
      while (batcher_ >> batch) {
        translators_[0].translate(batch);
      }
    }
  }

  return tracker;
}

void Service::cancel(RequestTracker *requestTracker) {
  std::async([&]() { batcher_.cancel(requestTracker); });
}

void Service::amend(RequestTracker *requestTracker, int nice) {
  std::async([&]() { batcher_.amend(requestTracker, nice); });
}

void Service::stop() {
  for (auto &worker : workers_) {
    Batch poison = Batch::poison();
    pcqueue_.ProduceSwap(poison);
  }

  for (auto &worker : workers_) {
    worker.join();
  }

  workers_.clear(); // Takes care of idempotency.
}

Service::~Service() { stop(); }

// Internal function nobody used, only within service.
std::vector<Ptr<const Vocab>> loadVocabularies(Ptr<Options> options) {
  // @TODO: parallelize vocab loading for faster startup
  auto vfiles = options->get<std::vector<std::string>>("vocabs");
  // with the current setup, we need at least two vocabs: src and trg
  ABORT_IF(vfiles.size() < 2, "Insufficient number of vocabularies.");
  std::vector<Ptr<Vocab const>> vocabs(vfiles.size());
  std::unordered_map<std::string, Ptr<Vocab>> vmap;
  for (size_t i = 0; i < vocabs.size(); ++i) {
    auto m = vmap.emplace(std::make_pair(vfiles[i], Ptr<Vocab>()));
    if (m.second) { // new: load the vocab
      m.first->second = New<Vocab>(options, i);
      m.first->second->load(vfiles[i]);
    }
    vocabs[i] = m.first->second;
  }
  return vocabs;
}

} // namespace bergamot
} // namespace marian
