#include "DataLog.h"
#include "MainServer.h"
#include "PerfLog.h"
#include "Timer.h"

#include "GpuIpc/IProtocol.h"
#include "GpuHandler/IGpuHandler.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include <Gaudi/PluginService.h>

using namespace boost;
using namespace std;

//--------
// helpers
//--------

template <typename T, size_t Size>
size_t arraySize(const T (&)[Size])
{
  return Size;
}

//----------
// interface
//----------

MainServer::MainServer(PerfLog & perfLog, DataLog & dataLog) :
    m_perfLog (perfLog),
    m_dataLog (dataLog) {
}

MainServer::~MainServer()
{
  for (auto & i : m_handlers)
    delete i.second;
}

//------------------------
// IProcess implementation
//------------------------

void MainServer::process(IProtocol & protocol) {
  const size_t FAIL_FLAG = 0xFFFFFFFF;

  std::string handlerName = protocol.readString();

  size_t size = protocol.readUInt32();

  Data input(size);
  protocol.readData(&input[0], size);

  HandlerMap::const_iterator i = m_handlers.find(handlerName);
  if (i == m_handlers.end()) {
    // when a handler is not found, inform the client
    protocol.writeUInt32(FAIL_FLAG);
    protocol.writeString(createInvalidHandlerMsg(handlerName));
    return;
  }

  // call the handler
  Data output;
  DataPacket packet(handlerName, &input, &output);
  m_dataQueue.push(&packet);

  Timer timer;
  timer.start();
  packet.Wait();
  timer.stop();

  // forward exceptions to client
  if (packet.ExceptionThrown()) {
    protocol.writeUInt32(FAIL_FLAG);
    protocol.writeString(packet.ExceptionMessage());
    return;
  }

  m_dataLog.addRecord(handlerName, input, output);

  // return the output to client
  protocol.writeUInt32(output.size());
  protocol.writeData(&output[0], output.size());
  protocol.writeDouble(timer.secondsElapsed());
}

void MainServer::start() {
    m_processingThread = thread(&MainServer::processQueue, this);
}

void MainServer::stop() {
  m_dataQueue.interrupt();
}

void MainServer::loadHandler(const string & handlerName) {
  IGpuHandler * handler = IGpuHandler::Factory::create(handlerName);
  if (!handler) {
    ostringstream msg;
    msg << "could not load handler '" << handlerName << "'";
    throw runtime_error(msg.str());
  }
  {
    scoped_lock lock(m_mutex);

    // handle repeated loading of the same handler
    HandlerMap::const_iterator i = m_handlers.find(handlerName);
    if (i != m_handlers.end())
      delete i->second;

    m_handlers[handlerName] = handler;
  }
  cout << "loaded handler '" << handlerName << "'\n";
}

//------------------
// Private functions
//------------------

size_t MainServer::addSize(size_t total, const Data * data) {
  return total + data->size();
}

uint8_t * MainServer::allocVector(
    size_t index,
    size_t size,
    IGpuHandler::AllocParam param) {
  if (size == 0)
    return nullptr;
  typedef vector<Data*> Batch;
  Batch * batch = reinterpret_cast<Batch*>(param);
  Data * data = batch->at(index);
  data->resize(size);
  return &data->at(0);
}

string MainServer::createInvalidHandlerMsg(const string & handler) const {
    ostringstream msg;
    msg << "invalid handler name: " << handler << "; ";
    msg << "valid handlers: ";
    bool first = true;
    for (const auto & i : m_handlers) {
      if (first)
        first = false;
      else
        msg << ", ";
      msg << i.first;
    }
    msg << ".";
    return msg.str();
}

IGpuHandler * MainServer::getHandlerByName(const std::string & name) {
  scoped_lock lock(m_mutex);
  HandlerMap::const_iterator i = m_handlers.find(name);
  if (i == m_handlers.end())
    throw runtime_error(createInvalidHandlerMsg(name));
  return i->second;
}

void MainServer::processQueue()
try {
  // the data queue throws an exception when interrupted
  while (true) {
    string              name;
    vector<DataPacket*> batch;
    m_dataQueue.pop(name, batch);

    IGpuHandler * handler = getHandlerByName(name);

    // prepare data
    vector<const Data*> input  (batch.size());
    vector<Data*>       output (batch.size());
    for (size_t i = 0, size = batch.size(); i != size; ++i) {
      input[i]  = batch[i]->Input();
      output[i] = batch[i]->Output();
    }

    Timer timer;

    try {
      // execute handler
      timer.start();
      (*handler)(input, allocVector, &output);
      timer.stop();
    } catch (const std::exception & e) {
      // propagate the exception to all client in the batch
      for (size_t i = 0, size = batch.size(); i != size; ++i)
        batch[i]->SetExceptionMessage(e.what());
    }

    // gather statistics
    double secondsElapsed  = timer.secondsElapsed();
    size_t totalInputSize  = accumulate(input.begin(),  input.end(),  0u, addSize);
    size_t totalOutputSize = accumulate(output.begin(), output.end(), 0u, addSize);
    m_perfLog.addRecord(
        time(0), name.c_str(), secondsElapsed,
        totalInputSize, totalOutputSize, batch.size());

    // wake up the clients
    for (size_t i = 0, size = batch.size(); i != size; ++i)
      batch[i]->Signal();

    // wait for them to finish before moving onto the next batch
  }
} catch (const Queue::InterruptedError &) {
  // it's ok
  // someone wants us to terminate quickly
}
