#pragma once

#include <map>
#include <vector>
#include "types.h"
#include "cams/icam.h"
#include "com_out/irequest_handler.h"
// Needed to store control data for video cam...
#include "output/storage.h"


namespace data_reader {
  class SensorStorage {
  public:
    SensorStorage(com_out::IRequestHandler& requestHandler, const TS& algoStartTime, output::Storage& outputStorage);

    typedef std::map<const std::string, std::shared_ptr<ICam>> CamMap;

    void initFromConfig(const std::string& filepath);
    // If key is left empty, a unique key will be generated
    // otherwise the user must guarantee the uniqueness of the key
    std::string addCam(std::shared_ptr<ICam> cam, std::string camKey = "");

    const CamMap& getCams() const { return _cams; };

  private:
    output::Storage& _outputStorage;

    CamMap _cams;
    unsigned int _sensorCounter; // used to create unique sensor ids
    const TS& _algoStartTime;

    com_out::IRequestHandler& _requestHandler;
  };
}
