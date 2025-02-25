#include "CaribouEvent2StdEventConverter.hh"
#include "eudaq/Exception.hh"

#include "H2MFrameDecoder.hpp"
#include "h2m_pixels.hpp"
#include "utils/log.hpp"

#include <string>
#include <algorithm>

using namespace eudaq;

namespace {
  auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<
      H2MEvent2StdEventConverter>(H2MEvent2StdEventConverter::m_id_factory);
}


bool H2MEvent2StdEventConverter::Converting(
    eudaq::EventSPC d1, eudaq::StandardEventSP d2,
    eudaq::ConfigurationSPC conf) const {
  auto ev = std::dynamic_pointer_cast<const eudaq::RawEvent>(d1);

  // No event
  if (!ev) {
    return false;
  }

  // Set eudaq::StandardPlane::ID for multiple detectors
  uint32_t plane_id = conf->Get("plane_id", 0);
  EUDAQ_DEBUG("Setting eudaq::StandardPlane::ID to " + to_string(plane_id));

  // Read acquisition mode from configuration, defaulting to ToT.
  uint8_t acq_mode = conf->Get("acq_mode", 0x1);

  // get an instance of the frame decoder
  static caribou::H2MFrameDecoder decoder;

  // Data container:
  std::vector<uint32_t> rawdata;

  // Retrieve data from event
  if (ev->NumBlocks() == 1) {

    // contains all data, split it into timestamps and pixel data, returned as
    // std::vector<uint8_t>
    auto datablock = ev->GetBlock(0);

    // get number of words in datablock
    auto data_length = datablock.size();

    // translate data block into pearyRawData format
    rawdata.resize(data_length / sizeof(uint32_t));
    memcpy(&rawdata[0], &datablock[0], data_length);

    // print some info
    EUDAQ_DEBUG("Data block contains " + to_string(data_length) + " words.");

  } // data from good event
  else {
    EUDAQ_WARN("Ignoring bad event " + to_string(ev->GetEventNumber()));
    return false;
  } // bad event

  EUDAQ_DEBUG("Length of rawdata (should be 262): " +to_string(rawdata.size()));
  //  first decode the header
  auto [ts_trig, ts_sh_open, ts_sh_close, frameID, length, t0] = decoder.decodeHeader<uint32_t>(rawdata);

  if(t0 == false || ts_sh_close < ts_sh_open) {
      EUDAQ_DEBUG("No T0 signal seen yet, skipping event");
      return false;
  }

  // remove the 6 elements from the header
  rawdata.erase(rawdata.begin(),rawdata.begin()+6);

  // Decode the event raw data - no zero suppression
  auto frame = decoder.decodeFrame<uint32_t>(rawdata, acq_mode);


  // Create a StandardPlane representing one sensor plane
  eudaq::StandardPlane plane(plane_id, "Caribou", "H2M");
  // go through pixels and add info to plane
  plane.SetSizeZS(64, 16, 0);
  // start and end are in 100MHz units -> to ps
  int _100MHz_to_ps = 10000;
  uint64_t frameStart = ts_sh_open*_100MHz_to_ps;
  uint64_t frameEnd = ts_sh_close*_100MHz_to_ps;
  EUDAQ_DEBUG("FrameID: "+ to_string(frameID) +"\t Shutter open: "+to_string(frameStart)+"\t shutter close "+to_string(frameEnd) +"\t t_0: "+to_string(t0));
  for (const auto &pixel : frame) {

    // get pixel information
    // pearydata is a map of a pair (col,row) and a pointer to a h2m_pixel
    auto [col, row] = pixel.first;

    // cast into right type of pixel and retrieve stored data
    auto pixHit = dynamic_cast<caribou::h2m_pixel_readout *>(pixel.second.get());

    // Pixel value of whatever equals zero means: no hit
    if(pixHit->GetData() == 0) {
      continue;
    }

    // Fetch timestamp from pixel if on ToA mode, otherwise set to frame center:
    uint64_t timestamp = (pixHit->GetMode() == caribou::ACQ_MODE_TOA ? (frameEnd - (pixHit->GetToA() * _100MHz_to_ps)) : ((frameStart + frameEnd) / 2));
    uint64_t tot = pixHit->GetToT();

    // assemble pixel and add to plane
    plane.PushPixel(col, row, tot, timestamp);
  } // pixels in frame

  // Add the plane to the StandardEvent
  d2->AddPlane(plane);

  // Store frame begin and end in picoseconds
  d2->SetTimeBegin(frameStart);
  d2->SetTimeEnd(frameEnd);
  // The frame ID is not necessarily related to a trigger but rather to an event ID:
  d2->SetEventN(frameID);

  // Identify the detetor type
  d2->SetDetectorType("H2M");

  // Copy tags - guess we have non for now, but just keep them here
  for (const auto &tag : d1->GetTags()) {
    d2->SetTag(tag.first, tag.second);
  }

  // Indicate that data was successfully converted
  return true;
}

