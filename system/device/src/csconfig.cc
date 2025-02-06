/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "bt_cs_config"
#include "device/include/csconfig.h"
#include <bluetooth/log.h>
#include <stack>
#include "btcore/include/module.h"
#include "osi/include/config.h"
#include "osi/include/future.h"
#include <libxml/parser.h>
#include "osi/include/properties.h"

using namespace bluetooth;
void parsecsProcedureSettings(xmlNode *);
void parsecsConfigSettings(xmlNode *);
void ReadLocalConfigs(void);
void print_cs_configs(void);
bool is_leaf(xmlNode *);
void convertStringToSubEventLen(std::string, uint8_t *);
void convertStringToPreferredAnt(std::string, uint8_t *);
void convertStringToChannelMap(std::string, uint8_t *);
void print_cs_procedure_settings(void);
std::vector<tCS_PROCEDURE_PARAM> cs_procedure_settings;
std::vector<tCS_CONFIG> cs_config_settings;
unsigned long cs_config_settings_count, cs_procedure_settings_count;
bool is_leaf(xmlNode *node) {
  xmlNode *child = node->children;
  while(child)
  {
    if (child->type == XML_ELEMENT_NODE)
      return false;

    child = child->next;
  }

  return true;
}

void convertStringToChannelMap (std::string content, uint8_t *channelMap) {
    size_t pos = 0;
    std::string token;
    std::vector<std::string> res;
    while ((pos = content.find(" ")) != std::string::npos) {
        token = content.substr(0, pos);
        log::info("token : {}\n", token.c_str());
        content.erase(0, pos + 1);
        res.push_back(token);
    }
    res.push_back(content);
    for (size_t i=0; i<res.size(); i++) {
        channelMap[i] = atoi(res[i].c_str());
    }

}

void convertStringToPreferredAnt (std::string content, uint8_t *preferred_peer_antenna) {
    size_t pos = 0;
    std::string token;
    std::vector<std::string> res;
    while ((pos = content.find(" ")) != std::string::npos) {
        token = content.substr(0, pos);
        log::info("token :{}\n", token.c_str());
        content.erase(0, pos + 1);
        res.push_back(token);
    }
    res.push_back(content);

    uint8_t temp_preferred_peer_antenna = 0;
    for (size_t i=0; i<res.size(); i++) {
      temp_preferred_peer_antenna = (temp_preferred_peer_antenna | ((atoi(res[i].c_str()) << i)));
    }
    *preferred_peer_antenna = temp_preferred_peer_antenna;
}

void convertStringToSubEventLen (std::string content, uint8_t *subEventLen) {
    size_t pos = 0;
    std::string token;
    std::vector<std::string> res;
    while ((pos = content.find(" ")) != std::string::npos) {
        token = content.substr(0, pos);
        log::info("token :{}\n", token.c_str());
        content.erase(0, pos + 1);
        res.push_back(token);
    }
    res.push_back(content);
    for (size_t i=0; i<res.size(); i++) {
        subEventLen[i] = atoi(res[i].c_str());
        log::info("subEventLen {}: {}\n", i, subEventLen[i]);
    }
}

void print_cs_procedure_settings() {
    log::info("size : {}\n", (int)cs_procedure_settings.size());
    for (int i=0; i<(int)cs_procedure_settings.size(); i++) {
        log::info("***** CS_PROC_SETTINGS {} START ****************\n", i);
        log::info("ConfigId: {}\n", cs_procedure_settings[i].config_id);
        log::info("max_proc_duration: {}\n", cs_procedure_settings[i].max_proc_duration);
        log::info("min_period_between_proc: {}\n", cs_procedure_settings[i].min_period_between_proc);
        log::info("max_period_between_proc: {}\n", cs_procedure_settings[i].max_period_between_proc);
        log::info("max_proc_count: {}\n", cs_procedure_settings[i].max_proc_count);

        for (size_t i=0; i< CS_SUBEVENT_LEN_SIZE; i++) {
            log::info("min_subevent_len[{}]: %x\n", i, cs_procedure_settings[i].min_subevent_len[i]);
        }
        for (size_t i=0; i<CS_SUBEVENT_LEN_SIZE; i++) {
            log::info("max_subevent_len[{}]: %x\n", i, cs_procedure_settings[i].max_subevent_len[i]);
        }
        log::info("tone_ant_cfg_selection: {}\n", cs_procedure_settings[i].tone_ant_cfg_selection);
        log::info("phy: {}\n", cs_procedure_settings[i].phy);
        log::info("tx_pwr_delta: {}\n", cs_procedure_settings[i].tx_pwr_delta);
	log::info("preferred_peer_antenna : {} \n", cs_procedure_settings[i].preferred_peer_antenna);
	log::info("snr_control_initiator : {} \n", cs_procedure_settings[i].snr_control_initiator);
	log::info("snr_control_reflector : {} \n", cs_procedure_settings[i].snr_control_reflector);
        log::info("***** CS_PROC_SETTINGS {} END ****************\n", i);
    }
}

void parsecsConfigSettings(xmlNode *input_node) {
   unsigned int TempFieldsCount = 0;
   std::stack<xmlNode*> profile_node_stack;
   unsigned int TempConfigsCount = 0;
   xmlNode *FirstChild = xmlFirstElementChild(input_node);
   unsigned long CsConfigFields = xmlChildElementCount(FirstChild);
   tCS_CONFIG temp_cs_config;
   memset(&temp_cs_config, 0, sizeof(tCS_CONFIG));

   log::info("cs Fields count is {} \n", CsConfigFields);
   for (xmlNode *node = input_node->children;
                 node != NULL || !profile_node_stack.empty(); node = node->children) {
     if (node == NULL) {
       node = profile_node_stack.top();
       profile_node_stack.pop();
     }

     if (node) {
       if (node->type == XML_ELEMENT_NODE) {
         if ((is_leaf(node))) {
           std::string content = (const char*)(xmlNodeGetContent(node));
           if (content[0] == '\0') {
                  log::info("Done with elements");
               return;
           }
           //log::info("name: %s\n", node->name);
           log::info("content: {}\n", content.c_str());
           if (!xmlStrcmp(node->name,(const xmlChar*)"ConfigId")) {
             temp_cs_config.config_id = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MainModeType")) {
             temp_cs_config.main_mode_type = (float)atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"SubModeType")) {
             temp_cs_config.sub_mode_type = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MainModeMinSteps")) {
             temp_cs_config.main_mode_min_steps = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MainModeMaxSteps")) {
             temp_cs_config.main_mode_max_steps = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MainModeRepetition")) {
             temp_cs_config.main_mode_rep = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"Mode0Steps")) {
             temp_cs_config.mode_0_steps = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"Role")) {
             temp_cs_config.role = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"RTTTypes")) {
             temp_cs_config.rtt_types = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"CsSyncPhy")) {
             temp_cs_config.cs_sync_phy = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"ChannelMap")) {
             convertStringToChannelMap(content, temp_cs_config.channel_map);
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"ChannelMapRepetition")) {
             temp_cs_config.channel_map_rep = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"ChannelSelectionType")) {
             temp_cs_config.hop_algo_type = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"ChannelShape")) {
             temp_cs_config.user_shape = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"ChannelJump")) {
             temp_cs_config.user_channel_jump = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"CompanionSignalEnable")) {
             temp_cs_config.comp_signal_enable = atoi(content.c_str());
             TempFieldsCount++;
           }
         }

         if (TempFieldsCount == CsConfigFields) {
            log::info("Done with {} many elements", CsConfigFields);
            cs_config_settings.push_back(temp_cs_config);
            memset(&temp_cs_config, 0, sizeof(tCS_CONFIG));
            TempFieldsCount = 0;
            TempConfigsCount++;
         }
         }
       }

       if (node->next != NULL)
       {
         profile_node_stack.push(node->next);
         node = node->next;
       }
   }

   log::info("All CS Configs are parsed successfully\n");
   print_cs_configs();
}

void parsecsProcedureSettings(xmlNode *input_node) {
   std::stack<xmlNode*> profile_node_stack;
   unsigned int TempCodecCount = 0;
   unsigned int TempFieldsCount = 0;
   xmlNode *FirstChild = xmlFirstElementChild(input_node);
   unsigned long CsProcedureFields = xmlChildElementCount(FirstChild);
   tCS_PROCEDURE_PARAM temp_cs_proc_param;
   memset(&temp_cs_proc_param, 0, sizeof(tCS_PROCEDURE_PARAM));

   log::info("cs procedure Fields count is %ld \n", CsProcedureFields);
   for (xmlNode *node = input_node->children;
                 node != NULL || !profile_node_stack.empty(); node = node->children) {
     if (node == NULL) {
       node = profile_node_stack.top();
       profile_node_stack.pop();
     }

     if (node) {
       if (node->type == XML_ELEMENT_NODE) {
         if ((is_leaf(node))) {
           std::string content = (const char*)(xmlNodeGetContent(node));
           if (content[0] == '\0') {
               return;
           }
	   log::info("Done with elements");
           if (!xmlStrcmp(node->name,(const xmlChar*)"MaxProcedureDuration")) {
             temp_cs_proc_param.max_proc_duration = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MinPeriodBetweenProcedures")) {
             temp_cs_proc_param.min_period_between_proc = (float)atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MaxPeriodBetweenProcedures")) {
             temp_cs_proc_param.max_period_between_proc = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MaxProcedureCount")) {
             temp_cs_proc_param.max_proc_count = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MinSubEventLen")) {
             convertStringToSubEventLen(content, temp_cs_proc_param.min_subevent_len);
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"MaxSubEventLen")) {
             convertStringToSubEventLen(content, temp_cs_proc_param.max_subevent_len);
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"ToneAntennaConfigSelection")) {
             temp_cs_proc_param.tone_ant_cfg_selection = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"Phy")) {
             temp_cs_proc_param.phy = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"TxPowerDelta")) {
             temp_cs_proc_param.tx_pwr_delta = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"PreferredPeerAntenna")) {
	     convertStringToPreferredAnt(content, &temp_cs_proc_param.preferred_peer_antenna);
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"SnrControlInitiator")) {
             temp_cs_proc_param.snr_control_initiator = atoi(content.c_str());
             TempFieldsCount++;
           } else if (!xmlStrcmp(node->name, (const xmlChar*)"SnrControlReflector")) {
             temp_cs_proc_param.snr_control_reflector = atoi(content.c_str());
             TempFieldsCount++;
           }
	 }

         if (TempFieldsCount == CsProcedureFields) {
             cs_procedure_settings.push_back(temp_cs_proc_param);
               TempFieldsCount = 0;
         }
       }
     }

     if (node->next != NULL) {
       profile_node_stack.push(node->next);
       node = node->next;
     }
   } // end of if (node)

   log::info("All CS Procedure Settings are parsed successfully\n");
   print_cs_procedure_settings();
}

void ReadLocalConfigs(void)
{
  xmlDoc *doc = NULL;
  xmlNode *root_element = NULL;
  bool local_config = false;

  char value[PROPERTY_VALUE_MAX];
  if (osi_property_get("persist.vendor.service.bt.config.local", value, "false")) {
      if (strncmp(value, "true", PROPERTY_VALUE_MAX) == 0)
       local_config = true;
  }
  if (!local_config)
  doc = xmlReadFile(CS_CONFIG_PATH, NULL, 0);
  else
    doc = xmlReadFile(CS_CONFIG_PATH_LOCAL, NULL, 0);
  if (doc == NULL) {
    log::error("Could not parse the CS XML file {}", CS_CONFIG_PATH);
    return;
  }

  root_element = xmlDocGetRootElement(doc);
  std::stack<xmlNode*> S;
  for (xmlNode *node = root_element; node != NULL || !S.empty(); node = node->children) {
    if (node == NULL) {
       node = S.top();
       S.pop();
    }

    if (node) {
      if (node->type == XML_ELEMENT_NODE) {
        if (!(is_leaf(node))) {
          std::string content = (const char *) (xmlNodeGetContent (node));
          if (content[0] == '\0') {
              return;
          }

          if (!xmlStrcmp (node->name, (const xmlChar *) "CsConfigurationList")) {
             log::info("CsConfigurationList configs being parsed\n");
          }

          if (!xmlStrcmp (node->name, (const xmlChar *) "CsConfig")) {
             log::info("CsConfig  being parsed\n");
             cs_config_settings_count = xmlChildElementCount(node);
             parsecsConfigSettings(node);
          }

          if (!xmlStrcmp (node->name, (const xmlChar *) "CsProcedure")) {
             log::info("CsProcedure configs being parsed\n");
             cs_procedure_settings_count = xmlChildElementCount(node);
             parsecsProcedureSettings(node);
          }
        }
      }

      if (node->next != NULL) {
        S.push(node -> next);
      }
    }
  }
  xmlFreeDoc(doc);
  xmlCleanupParser();
}

void print_cs_configs() {
    log::info("size : {}\n", (int)cs_config_settings.size());
    for (int i=0; i<(int)cs_config_settings.size(); i++) {
        log::info("***** CS_CONFIG {} START ****************\n", i);
        log::info("ConfigId: {}\n", cs_config_settings[i].config_id);
        log::info("MainModeType: {}\n", cs_config_settings[i].main_mode_type);
        log::info("SubModeType: {}\n", cs_config_settings[i].sub_mode_type);
        log::info("MainModeMinSteps: {}\n", cs_config_settings[i].main_mode_min_steps);
        log::info("MainModeMaxSteps: {}\n", cs_config_settings[i].main_mode_max_steps);
        log::info("MainModeRepetition: {}\n", cs_config_settings[i].main_mode_rep);
        log::info("Mode0Steps: {}\n", cs_config_settings[i].mode_0_steps);
        log::info("Role: {}\n", cs_config_settings[i].role);

        log::info("RTTTypes: {}\n", cs_config_settings[i].rtt_types);
        log::info("CsSyncPhy: {}\n", cs_config_settings[i].cs_sync_phy);
        for (size_t i=0; i<CS_CHANNEL_MAP_SIZE; i++) {
            log::info("channelMap{}: {}\n", i, cs_config_settings[i].channel_map[i]);
        }
        log::info("ChannelMapRepetition: {}\n", cs_config_settings[i].channel_map_rep);
        log::info("ChannelSelectionType: {}\n", cs_config_settings[i].hop_algo_type);
        log::info("ChannelShape: {}\n", cs_config_settings[i].user_shape);
        log::info("ChannelJump: {}\n", cs_config_settings[i].user_channel_jump);
        log::info("CompanionSignalEnable: {}\n", cs_config_settings[i].comp_signal_enable);
        log::info("***** CS_CONFIG {} END ****************\n", i);
    }
}

bool get_cs_config_settings(int index, tCS_CONFIG *cs_config_setting) {
    if (index >= (int)cs_config_settings.size()) {
      log::warn("selected procedure parameters are not available in config");
      return false;
    }

    cs_config_setting->config_id = cs_config_settings[index].config_id;
    cs_config_setting->main_mode_type = cs_config_settings[index].main_mode_type;
    cs_config_setting->sub_mode_type =  cs_config_settings[index].sub_mode_type;
    cs_config_setting->main_mode_min_steps = cs_config_settings[index].main_mode_min_steps;
    cs_config_setting->main_mode_max_steps = cs_config_settings[index].main_mode_max_steps;
    cs_config_setting->main_mode_rep = cs_config_settings[index].main_mode_rep;
    cs_config_setting->mode_0_steps = cs_config_settings[index].mode_0_steps;
    cs_config_setting->role = cs_config_settings[index].role;
    cs_config_setting->rtt_types = cs_config_settings[index].rtt_types;

    cs_config_setting->cs_sync_phy = cs_config_settings[index].cs_sync_phy;
    for (size_t i=0; i<CS_CHANNEL_MAP_SIZE; i++) {
        cs_config_setting->channel_map[i] = cs_config_settings[index].channel_map[i];
    }
    cs_config_setting->channel_map_rep = cs_config_settings[index].channel_map_rep;
    cs_config_setting->hop_algo_type = cs_config_settings[index].hop_algo_type;
    cs_config_setting->user_shape = cs_config_settings[index].user_shape;
    cs_config_setting->cs_sync_phy = cs_config_settings[index].cs_sync_phy;
    cs_config_setting->user_channel_jump = cs_config_settings[index].user_channel_jump;
    cs_config_setting->comp_signal_enable = cs_config_settings[index].comp_signal_enable;
    return true;
}

bool get_cs_procedure_settings(int index,
		tCS_PROCEDURE_PARAM *cs_proc_setting) {
    if (index >= (int)cs_procedure_settings.size()) {
      log::warn("selected procedure parameters are not available in config");
      return false;
    }

    log::warn("index :{}", index);
    cs_proc_setting->max_proc_duration = cs_procedure_settings[index].max_proc_duration;
    cs_proc_setting->min_period_between_proc = cs_procedure_settings[index].min_period_between_proc;
    cs_proc_setting->max_period_between_proc = cs_procedure_settings[index].max_period_between_proc;
    cs_proc_setting->max_proc_count = cs_procedure_settings[index].max_proc_count;
    for (size_t i=0; i<CS_SUBEVENT_LEN_SIZE; i++) {
        cs_proc_setting->min_subevent_len[i] = cs_procedure_settings[index].min_subevent_len[i];
    }
    for (size_t i=0; i<CS_SUBEVENT_LEN_SIZE; i++) {
        cs_proc_setting->max_subevent_len[i] = cs_procedure_settings[index].max_subevent_len[i];
    }
    cs_proc_setting->tone_ant_cfg_selection = cs_procedure_settings[index].tone_ant_cfg_selection;
    cs_proc_setting->phy = cs_procedure_settings[index].phy;
    cs_proc_setting->tx_pwr_delta = cs_procedure_settings[index].tx_pwr_delta;
    cs_proc_setting->preferred_peer_antenna = cs_procedure_settings[index].preferred_peer_antenna;
    cs_proc_setting->snr_control_initiator = cs_procedure_settings[index].snr_control_initiator;
    cs_proc_setting->snr_control_reflector = cs_procedure_settings[index].snr_control_reflector;
    return true;
}

future_t* cs_config_module_init(void) {
  log::info("");
  cs_config_settings_count = 0;
  cs_procedure_settings_count = 0;
  ReadLocalConfigs();
  return future_new_immediate(FUTURE_SUCCESS);
}

future_t* cs_config_module_clean_up(void) {
  log::info("");
  return future_new_immediate(FUTURE_SUCCESS);
}

EXPORT_SYMBOL module_t cs_config_module = {
    .name = CS_CONFIG_MODULE,
    .init = cs_config_module_init,
    .start_up = NULL,
    .shut_down = NULL,
    .clean_up = cs_config_module_clean_up};
