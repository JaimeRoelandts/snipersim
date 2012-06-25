#include "simulator.h"
#include "log.h"
#include "core.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "sync_server.h"
#include "syscall_server.h"
#include "magic_server.h"
#include "sim_thread_manager.h"
#include "clock_skew_minimization_object.h"
#include "fxsupport.h"
#include "timer.h"
#include "stats.h"
#include "pthread_emu.h"
#include "trace_manager.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "instruction.h"
#include "config.hpp"
#include "magic_client.h"

#include <sstream>

Simulator *Simulator::m_singleton;
config::Config *Simulator::m_config_file;
bool Simulator::m_config_file_allowed = true;
Config::SimulationMode Simulator::m_mode;

static UInt64 getTime()
{
   timeval t;
   gettimeofday(&t, NULL);
   UInt64 time = (((UInt64)t.tv_sec) * 1000000 + t.tv_usec);
   return time;
}

void Simulator::allocate()
{
   assert(m_singleton == NULL);
   m_singleton = new Simulator();
}

void Simulator::setConfig(config::Config *cfg, Config::SimulationMode mode)
{
   m_config_file = cfg;
   m_mode = mode;
}

void Simulator::release()
{
   // Fxsupport::fini();
   delete m_singleton;
   m_singleton = NULL;
}

Simulator::Simulator()
   : m_config(m_mode)
   , m_log(m_config)
   , m_stats_manager(new StatsManager)
   , m_transport(NULL)
   , m_core_manager(NULL)
   , m_thread_manager(NULL)
   , m_sim_thread_manager(NULL)
   , m_clock_skew_minimization_manager(NULL)
   , m_trace_manager(NULL)
   , m_dvfs_manager(NULL)
   , m_hooks_manager(NULL)
   , m_boot_time(getTime())
   , m_start_time(0)
   , m_stop_time(0)
   , m_shutdown_time(0)
{
}

void Simulator::start()
{
   LOG_PRINT("In Simulator ctor.");

   m_hooks_manager = new HooksManager();
   m_syscall_server = new SyscallServer();
   m_sync_server = new SyncServer();
   m_magic_server = new MagicServer();
   m_transport = Transport::create();
   m_dvfs_manager = new DvfsManager();
   m_thread_manager = new ThreadManager();
   m_core_manager = new CoreManager();
   m_sim_thread_manager = new SimThreadManager();
   m_clock_skew_minimization_manager = ClockSkewMinimizationManager::create();
   m_clock_skew_minimization_server = ClockSkewMinimizationServer::create();

   if (Sim()->getCfg()->getBool("traceinput/enabled"))
      m_trace_manager = new TraceManager();
   else
      m_trace_manager = NULL;

   Fxsupport::init();

   PthreadEmu::init();

   m_hooks_manager->init();

   m_sim_thread_manager->spawnSimThreads();

   Instruction::initializeStaticInstructionModel();

   InstMode::inst_mode_init = InstMode::fromString(getCfg()->getString("general/inst_mode_init"));
   InstMode::inst_mode_roi  = InstMode::fromString(getCfg()->getString("general/inst_mode_roi"));
   InstMode::inst_mode_end  = InstMode::fromString(getCfg()->getString("general/inst_mode_end"));
   setInstrumentationMode(InstMode::inst_mode_init);

   /* Save a copy of the configuration for reference */
   m_config_file->saveAs(m_config.formatOutputFileName("sim.cfg"));

// PIN_SpawnInternalThread doesn't schedule its threads until after PIN_StartProgram
//   m_transport->barrier();

   m_hooks_manager->callHooks(HookType::HOOK_SIM_START, 0);
   m_stats_manager->recordStatsBase();
   m_stats_manager->recordStats("start");
   if (Sim()->getConfig()->getSimulationROI() == Config::ROI_FULL)
   {
      // roi-begin
      enablePerformanceGlobal();
   }
}

Simulator::~Simulator()
{
   // Done with all the Pin stuff, allow using Config::Config again
   m_config_file_allowed = true;

   if (Sim()->getConfig()->getSimulationROI() == Config::ROI_FULL)
   {
      // roi-end
      disablePerformanceGlobal();
   }
   m_stats_manager->recordStats("stop");
   m_hooks_manager->callHooks(HookType::HOOK_SIM_END, 0);

   TotalTimer::reports();

   m_shutdown_time = getTime();

   LOG_PRINT("Simulator dtor starting...");

   m_hooks_manager->fini();

   if (m_clock_skew_minimization_manager)
      delete m_clock_skew_minimization_manager;
   if (m_clock_skew_minimization_server)
      delete m_clock_skew_minimization_server;

   m_sim_thread_manager->quitSimThreads();

   m_transport->barrier();

   #if 0
   if (Config::getSingleton()->getCurrentProcessNum() == 0)
   {
      std::ofstream os(Config::getSingleton()->getOutputFileName().c_str());

      os << "Simulation timers: " << std::endl
         << "start time\t" << (m_start_time - m_boot_time) << std::endl
         << "stop time\t" << (m_stop_time - m_boot_time) << std::endl
         << "shutdown time\t" << (m_shutdown_time - m_boot_time) << std::endl;

      m_core_manager->outputSummary(os);
      os.close();
   }
   else
   {
      std::stringstream temp;
      m_core_manager->outputSummary(temp);
      assert(temp.str().length() == 0);
   }
   #endif

   delete m_trace_manager;
   delete m_sim_thread_manager;
   delete m_thread_manager;
   delete m_core_manager;
   delete m_magic_server;
   delete m_transport;
}

void Simulator::startTimer()
{
   m_start_time = getTime();
}

void Simulator::stopTimer()
{
   m_stop_time = getTime();
}

void Simulator::enablePerformanceModels()
{
   Sim()->startTimer();
   for (UInt32 i = 0; i < Sim()->getConfig()->getTotalCores(); i++)
      Sim()->getCoreManager()->getCoreFromID(i)->enablePerformanceModels();
}

void Simulator::disablePerformanceModels()
{
   Sim()->stopTimer();
   for (UInt32 i = 0; i < Sim()->getConfig()->getTotalCores(); i++)
      Sim()->getCoreManager()->getCoreFromID(i)->disablePerformanceModels();
}

void Simulator::setInstrumentationMode(InstMode::inst_mode_t new_mode)
{
   if (Sim()->getConfig()->getSimulationMode() == Config::PINTOOL)
      InstMode::SetInstrumentationMode(new_mode);

   Sim()->getHooksManager()->callHooks(HookType::HOOK_INSTRUMENT_MODE, (UInt64)new_mode);
   getClockSkewMinimizationServer()->setDisable(new_mode != InstMode::DETAILED);
}
