/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2019,  The University of Memphis,
 *                           Regents of the University of California
 *
 * This file is part of NLSR (Named-data Link State Routing).
 * See AUTHORS.md for complete list of NLSR authors and contributors.
 *
 * NLSR is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NLSR is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NLSR, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include "hello-protocol.hpp"
#include "nlsr.hpp"
#include "lsdb.hpp"
#include "utility/name-helper.hpp"
#include "logger.hpp"

namespace nlsr {

INIT_LOGGER(HelloProtocol);

const std::string HelloProtocol::INFO_COMPONENT = "INFO";
const std::string HelloProtocol::NLSR_COMPONENT = "nlsr";

HelloProtocol::HelloProtocol(ndn::Face& face, ndn::KeyChain& keyChain,
                             ndn::security::SigningInfo& signingInfo,
                             ConfParameter& confParam, RoutingTable& routingTable,
                             Lsdb& lsdb)
  : m_face(face)
  , m_scheduler(m_face.getIoService())
  , m_keyChain(keyChain)
  , m_signingInfo(signingInfo)
  , m_confParam(confParam)
  , m_routingTable(routingTable)
  , m_lsdb(lsdb)
{
}

void
HelloProtocol::expressInterest(const ndn::Name& interestName, uint32_t seconds)
{
  NLSR_LOG_DEBUG("Expressing Interest :" << interestName);
  ndn::Interest interest(interestName);
  interest.setInterestLifetime(ndn::time::seconds(seconds));
  interest.setMustBeFresh(true);
  interest.setCanBePrefix(true);
  m_face.expressInterest(interest,
                         std::bind(&HelloProtocol::onContent, this, _1, _2),
                         [this] (const ndn::Interest& interest, const ndn::lp::Nack& nack)
                         {
                           NDN_LOG_TRACE("Received Nack with reason " << nack.getReason());
                           NDN_LOG_TRACE("Treating as timeout");
                           processInterestTimedOut(interest);
                         },
                         std::bind(&HelloProtocol::processInterestTimedOut, this, _1));

  // increment SENT_HELLO_INTEREST
  hpIncrementSignal(Statistics::PacketType::SENT_HELLO_INTEREST);
}

void
HelloProtocol::sendScheduledInterest()
{
  for (const auto& adjacent : m_confParam.getAdjacencyList().getAdjList()) {
    // If this adjacency has a Face, just proceed as usual.
    if(adjacent.getFaceId() != 0) {
      // interest name: /<neighbor>/NLSR/INFO/<router>
      ndn::Name interestName = adjacent.getName() ;
      interestName.append(NLSR_COMPONENT);
      interestName.append(INFO_COMPONENT);
      interestName.append(m_confParam.getRouterPrefix().wireEncode());
      expressInterest(interestName, m_confParam.getInterestResendTime());
      NLSR_LOG_DEBUG("Sending scheduled interest: " << interestName);
    }
  }
  scheduleInterest(m_confParam.getInfoInterestInterval());
}

void
HelloProtocol::scheduleInterest(uint32_t seconds)
{
  NLSR_LOG_DEBUG("Scheduling HELLO Interests in " << ndn::time::seconds(seconds));

  m_scheduler.schedule(ndn::time::seconds(seconds), [this] { sendScheduledInterest(); });
}

void
HelloProtocol::processInterest(const ndn::Name& name,
                               const ndn::Interest& interest)
{
  // interest name: /<neighbor>/NLSR/INFO/<router>
  const ndn::Name interestName = interest.getName();

  // increment RCV_HELLO_INTEREST
  hpIncrementSignal(Statistics::PacketType::RCV_HELLO_INTEREST);

  NLSR_LOG_DEBUG("Interest Received for Name: " << interestName);
  if (interestName.get(-2).toUri() != INFO_COMPONENT) {
    NLSR_LOG_DEBUG("INFO_COMPONENT not found or interestName: " << interestName
               << " does not match expression");
    return;
  }

  ndn::Name neighbor;
  neighbor.wireDecode(interestName.get(-1).blockFromValue());
  NLSR_LOG_DEBUG("Neighbor: " << neighbor);
  if (m_confParam.getAdjacencyList().isNeighbor(neighbor)) {
    std::shared_ptr<ndn::Data> data = std::make_shared<ndn::Data>();
    data->setName(ndn::Name(interest.getName()).appendVersion());
    data->setFreshnessPeriod(ndn::time::seconds(10)); // 10 sec
    data->setContent(reinterpret_cast<const uint8_t*>(INFO_COMPONENT.c_str()),
                                                      INFO_COMPONENT.size());

    m_keyChain.sign(*data, m_signingInfo);

    NLSR_LOG_DEBUG("Sending out data for name: " << interest.getName());

    m_face.put(*data);
    // increment SENT_HELLO_DATA
    hpIncrementSignal(Statistics::PacketType::SENT_HELLO_DATA);

    auto adjacent = m_confParam.getAdjacencyList().findAdjacent(neighbor);
    // If this neighbor was previously inactive, send our own hello interest, too
    if (adjacent->getStatus() == Adjacent::STATUS_INACTIVE) {
      // We can only do that if the neighbor currently has a face.
      if(adjacent->getFaceId() != 0){
        // interest name: /<neighbor>/NLSR/INFO/<router>
        ndn::Name interestName(neighbor);
        interestName.append(NLSR_COMPONENT);
        interestName.append(INFO_COMPONENT);
        interestName.append(m_confParam.getRouterPrefix().wireEncode());
        expressInterest(interestName, m_confParam.getInterestResendTime());
      }
    }
  }
}

void
HelloProtocol::processInterestTimedOut(const ndn::Interest& interest)
{
  // interest name: /<neighbor>/NLSR/INFO/<router>
  const ndn::Name interestName(interest.getName());
  NLSR_LOG_DEBUG("Interest timed out for Name: " << interestName);
  if (interestName.get(-2).toUri() != INFO_COMPONENT) {
    return;
  }
  ndn::Name neighbor = interestName.getPrefix(-3);
  NLSR_LOG_DEBUG("Neighbor: " << neighbor);
  m_confParam.getAdjacencyList().incrementTimedOutInterestCount(neighbor);

  Adjacent::Status status = m_confParam.getAdjacencyList().getStatusOfNeighbor(neighbor);

  uint32_t infoIntTimedOutCount =
    m_confParam.getAdjacencyList().getTimedOutInterestCount(neighbor);
  NLSR_LOG_DEBUG("Status: " << status);
  NLSR_LOG_DEBUG("Info Interest Timed out: " << infoIntTimedOutCount);
  if (infoIntTimedOutCount < m_confParam.getInterestRetryNumber()) {
    // interest name: /<neighbor>/NLSR/INFO/<router>
    ndn::Name interestName(neighbor);
    interestName.append(NLSR_COMPONENT);
    interestName.append(INFO_COMPONENT);
    interestName.append(m_confParam.getRouterPrefix().wireEncode());
    NLSR_LOG_DEBUG("Resending interest: " << interestName);
    expressInterest(interestName, m_confParam.getInterestResendTime());
  }
  else if ((status == Adjacent::STATUS_ACTIVE) &&
           (infoIntTimedOutCount == m_confParam.getInterestRetryNumber())) {
    m_confParam.getAdjacencyList().setStatusOfNeighbor(neighbor, Adjacent::STATUS_INACTIVE);

    NLSR_LOG_DEBUG("Neighbor: " << neighbor << " status changed to INACTIVE");

    m_lsdb.scheduleAdjLsaBuild();
  }
}

  // This is the first function that incoming Hello data will
  // see. This checks if the data appears to be signed, and passes it
  // on to validate the content of the data.
void
HelloProtocol::onContent(const ndn::Interest& interest, const ndn::Data& data)
{
  NLSR_LOG_DEBUG("Received data for INFO(name): " << data.getName());
  if (data.getSignature().hasKeyLocator()) {
    if (data.getSignature().getKeyLocator().getType() == ndn::KeyLocator::KeyLocator_Name) {
      NLSR_LOG_DEBUG("Data signed with: " << data.getSignature().getKeyLocator().getName());
    }
  }
  m_confParam.getValidator().validate(data,
                                      std::bind(&HelloProtocol::onContentValidated, this, _1),
                                      std::bind(&HelloProtocol::onContentValidationFailed,
                                                this, _1, _2));
}

void
HelloProtocol::onContentValidated(const ndn::Data& data)
{
  // data name: /<neighbor>/NLSR/INFO/<router>/<version>
  ndn::Name dataName = data.getName();
  NLSR_LOG_DEBUG("Data validation successful for INFO(name): " << dataName);

  if (dataName.get(-3).toUri() == INFO_COMPONENT) {
    ndn::Name neighbor = dataName.getPrefix(-4);

    Adjacent::Status oldStatus = m_confParam.getAdjacencyList().getStatusOfNeighbor(neighbor);
    m_confParam.getAdjacencyList().setStatusOfNeighbor(neighbor, Adjacent::STATUS_ACTIVE);
    m_confParam.getAdjacencyList().setTimedOutInterestCount(neighbor, 0);
    Adjacent::Status newStatus = m_confParam.getAdjacencyList().getStatusOfNeighbor(neighbor);

    NLSR_LOG_DEBUG("Neighbor : " << neighbor);
    NLSR_LOG_DEBUG("Old Status: " << oldStatus << " New Status: " << newStatus);
    // change in Adjacency list
    if ((oldStatus - newStatus) != 0) {
      if (m_confParam.getHyperbolicState() == HYPERBOLIC_STATE_ON) {
        m_routingTable.scheduleRoutingTableCalculation();
      }
      else {
        m_lsdb.scheduleAdjLsaBuild();
      }
    }
  }
  // increment RCV_HELLO_DATA
  hpIncrementSignal(Statistics::PacketType::RCV_HELLO_DATA);
}

void
HelloProtocol::onContentValidationFailed(const ndn::Data& data,
                                         const ndn::security::v2::ValidationError& ve)
{
  NLSR_LOG_DEBUG("Validation Error: " << ve);
}

} // namespace nlsr
