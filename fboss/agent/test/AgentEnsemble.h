// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <fboss/agent/gen-cpp2/agent_config_types.h>
#include <fboss/agent/gen-cpp2/platform_config_types.h>
#include <fboss/agent/gen-cpp2/switch_config_types.h>
#include <fboss/agent/hw/bcm/gen-cpp2/bcm_config_types.h>
#include <unistd.h>
#include <functional>
#include "fboss/agent/L2Entry.h"
#include "fboss/agent/Main.h"

#include "fboss/agent/FbossInit.h"
#include "fboss/agent/SwAgentInitializer.h"
#include "fboss/agent/SwSwitch.h"
#include "fboss/agent/hw/test/LinkStateToggler.h"
#include "fboss/agent/test/RouteDistributionGenerator.h"
#include "fboss/agent/test/TestEnsembleIf.h"

DECLARE_string(config);
DECLARE_bool(setup_for_warmboot);

namespace facebook::fboss {
class StateObserver;

using AgentEnsembleSwitchConfigFn = std::function<
    cfg::SwitchConfig(SwSwitch* swSwitch, const std::vector<PortID>&)>;
using AgentEnsemblePlatformConfigFn = std::function<void(cfg::PlatformConfig&)>;

class AgentEnsemble : public TestEnsembleIf {
 public:
  AgentEnsemble() {}
  explicit AgentEnsemble(const std::string& configFileName);
  ~AgentEnsemble();
  using TestEnsembleIf::masterLogicalPortIds;

  void setupEnsemble(
      uint32_t hwFeaturesDesired,
      AgentEnsembleSwitchConfigFn initConfig,
      AgentEnsemblePlatformConfigFn platformConfig =
          AgentEnsemblePlatformConfigFn());

  void startAgent();

  void applyNewConfig(const cfg::SwitchConfig& config, bool activate);

  void setupLinkStateToggler();

  SwSwitch* getSw() const {
    return agentInitializer()->sw();
  }

  std::shared_ptr<SwitchState> getProgrammedState() const override {
    return getSw()->getState();
  }

  const std::map<int32_t, cfg::PlatformPortEntry>& getPlatformPorts()
      const override {
    return getSw()->getPlatformMapping()->getPlatformPorts();
  }

  void applyInitialConfig(const cfg::SwitchConfig& config) override {
    // agent will start after writing an initial config.
    applyNewConfig(config, false);
  }

  std::shared_ptr<SwitchState> applyNewConfig(
      const cfg::SwitchConfig& config) override {
    applyNewConfig(config, true);
    return getProgrammedState();
  }

  virtual const SwAgentInitializer* agentInitializer() const = 0;
  virtual SwAgentInitializer* agentInitializer() = 0;
  virtual void createSwitch(
      std::unique_ptr<AgentConfig> config,
      uint32_t hwFeaturesDesired,
      PlatformInitFn initPlatform) = 0;
  virtual void reloadPlatformConfig() = 0;
  virtual bool isSai() const = 0;

  std::shared_ptr<SwitchState> applyNewState(
      std::shared_ptr<SwitchState> state,
      bool transaction = false) override;

  std::vector<PortID> masterLogicalPortIds() const override;

  void switchRunStateChanged(SwitchRunState runState) override;

  void programRoutes(
      const RouterID& rid,
      const ClientID& client,
      const utility::RouteDistributionGenerator::ThriftRouteChunks&
          routeChunks);

  void unprogramRoutes(
      const RouterID& rid,
      const ClientID& client,
      const utility::RouteDistributionGenerator::ThriftRouteChunks&
          routeChunks);

  void gracefulExit();

  static void enableExactMatch(bcm::BcmConfig& config);

  static std::string getInputConfigFile();

  void packetReceived(std::unique_ptr<RxPacket> pkt) noexcept override {
    getSw()->packetReceived(std::move(pkt));
  }

  void linkStateChanged(
      PortID port,
      bool up,
      std::optional<phy::LinkFaultStatus> iPhyFaultStatus =
          std::nullopt) override {
    if (getSw()->getSwitchRunState() >= SwitchRunState::CONFIGURED) {
      if (linkToggler_) {
        linkToggler_->linkStateChanged(port, up);
      }
    }
  }

  void l2LearningUpdateReceived(
      L2Entry l2Entry,
      L2EntryUpdateType l2EntryUpdateType) override {
    getSw()->l2LearningUpdateReceived(l2Entry, l2EntryUpdateType);
  }

  void exitFatal() const noexcept override {
    getSw()->exitFatal();
  }

  void pfcWatchdogStateChanged(const PortID& port, const bool deadlock)
      override {
    getSw()->pfcWatchdogStateChanged(port, deadlock);
  }

  std::map<PortID, HwPortStats> getLatestPortStats(
      const std::vector<PortID>& ports);

  HwPortStats getLatestPortStats(const PortID& port);

  void setLoopbackMode(cfg::PortLoopbackMode mode) {
    mode_ = mode;
  }

  const SwitchIdScopeResolver& scopeResolver() const override {
    CHECK(getSw()->getScopeResolver());
    return *(getSw()->getScopeResolver());
  }

  HwAsicTable* getHwAsicTable() override {
    return getSw()->getHwAsicTable();
  }

  std::map<PortID, FabricEndpoint> getFabricReachability() const override {
    return getSw()->getHwSwitchHandler()->getFabricReachability();
  }

  FabricReachabilityStats getFabricReachabilityStats() const override {
    return getSw()->getHwSwitchHandler()->getFabricReachabilityStats();
  }

  void updateStats() override {
    getSw()->updateStats();
  }

  void registerStateObserver(StateObserver* observer, const std::string& name)
      override;
  void unregisterStateObserver(StateObserver* observer) override;

 private:
  void writeConfig(const cfg::SwitchConfig& config);
  void writeConfig(const cfg::AgentConfig& config);
  void writeConfig(const cfg::AgentConfig& config, const std::string& file);

  cfg::SwitchConfig initialConfig_;
  std::unique_ptr<std::thread> asyncInitThread_{nullptr};
  std::vector<PortID> masterLogicalPortIds_;
  std::string configFile_{"agent.conf"};
  std::unique_ptr<LinkStateToggler> linkToggler_;
  cfg::PortLoopbackMode mode_{cfg::PortLoopbackMode::MAC};
};

int ensembleMain(int argc, char* argv[], PlatformInitFn initPlatform);

std::unique_ptr<AgentEnsemble> createAgentEnsemble(
    AgentEnsembleSwitchConfigFn initialConfigFn,
    AgentEnsemblePlatformConfigFn platformConfigFn =
        AgentEnsemblePlatformConfigFn(),
    uint32_t featuresDesired =
        (HwSwitch::FeaturesDesired::PACKET_RX_DESIRED |
         HwSwitch::FeaturesDesired::LINKSCAN_DESIRED));

} // namespace facebook::fboss
