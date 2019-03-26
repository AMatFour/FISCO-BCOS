/*
 * AMOPStorage.cpp
 *
 *  Created on: 2019年3月24日
 *      Author: monan
 */

#include "StorageException.h"

#include "AMOPStorage.h"
#include <libchannelserver/ChannelRPCServer.h>
#include <libdevcore/easylog.h>

#include "Common.h"
#include "Table.h"
#include <libchannelserver/ChannelMessage.h>

using namespace dev;
using namespace dev::storage;

AMOPStorage::AMOPStorage()
{
    _fatalHandler = [](std::exception& e) {
        (void)e;
        exit(-1);
    };
}

Entries::Ptr AMOPStorage::select(
    h256 hash, int num, const std::string& table, const std::string& key, Condition::Ptr condition)
{
    try
    {
        LOG(DEBUG) << "Query AMOPDB data";

        Json::Value requestJson;

        requestJson["op"] = "select";
        requestJson["params"]["blockHash"] = hash.hex();
        requestJson["params"]["num"] = num;
        requestJson["params"]["table"] = table;
        requestJson["params"]["key"] = key;

        for (auto it : *(condition->getConditions()))
        {
            Json::Value cond;
            cond.append(it.first);
            cond.append(it.second.first);
            cond.append(it.second.second);
            requestJson["params"]["condition"].append(cond);
        }

        Json::Value responseJson = requestDB(requestJson);

        int code = responseJson["code"].asInt();
        if (code != 0)
        {
            LOG(ERROR) << "Remote database return error:" << code;

            throw StorageException(
                -1, "Remote database return error:" + boost::lexical_cast<std::string>(code));
        }

        std::vector<std::string> columns;
        for (Json::ArrayIndex i = 0; i < responseJson["result"]["columns"].size(); ++i)
        {
            std::string fieldName = responseJson["result"]["columns"].get(i, "").asString();
            columns.push_back(fieldName);
        }

        Entries::Ptr entries = std::make_shared<Entries>();
        for (Json::ArrayIndex i = 0; i < responseJson["result"]["data"].size(); ++i)
        {
            Json::Value line = responseJson["result"]["data"].get(i, "");
            Entry::Ptr entry = std::make_shared<Entry>();

            for (Json::ArrayIndex j = 0; j < line.size(); ++j)
            {
                std::string fieldValue = line.get(j, "").asString();

                entry->setField(columns[j], fieldValue);
            }

            // entry->setStatus(boost::lexical_cast<int>(entry->getField(STATUS)));
            if (entry->getStatus() == 0)
            {
                entry->setDirty(false);
                entries->addEntry(entry);
            }
        }

        entries->setDirty(false);
        return entries;
    }
    catch (std::exception& e)
    {
        LOG(ERROR) << "Query database error:" << e.what();

        throw StorageException(-1, std::string("Query database error:") + e.what());
    }

    return Entries::Ptr();
}

size_t AMOPStorage::commit(
    h256 hash, int64_t num, const std::vector<TableData::Ptr>& datas, h256 const& blockHash)
{
    try
    {
        (void)blockHash;
        LOG(DEBUG) << "Commit data to database:" << datas.size();

        if (datas.size() == 0)
        {
            LOG(DEBUG) << "Empty data.";

            return 0;
        }

        Json::Value requestJson;

        requestJson["op"] = "commit";
        requestJson["params"]["blockHash"] = hash.hex();
        requestJson["params"]["num"] = num;

        for (auto it : datas)
        {
            Json::Value tableData;
            auto tableInfo = it->info;
            tableData["table"] = tableInfo->name;

            for (size_t i = 0; i < it->entries->size(); ++i)
            {
                auto entry = it->entries->get(i);

                Json::Value value;

                for (auto fieldIt : *entry->fields())
                {
                    value[fieldIt.first] = fieldIt.second;
                }

                tableData["entries"].append(value);

#if 0
    	  for (size_t i = 0; i < entriesIt.second->size(); ++i) {
		  if (entriesIt.second->get(i)->dirty()) {
			Json::Value value;
			for (auto fieldIt : *(entriesIt.second->get(i)->fields())) {
			  value[fieldIt.first] = fieldIt.second;
			}
			entry["values"].append(value);
		  }
		}
#endif
            }


#if 0
      for (auto entriesIt : it->entries) {
        if (entriesIt.second->dirty()) {
          Json::Value entry;
          entry["key"] = entriesIt.first;

          for (size_t i = 0; i < entriesIt.second->size(); ++i) {
            if (entriesIt.second->get(i)->dirty()) {
              Json::Value value;
              for (auto fieldIt : *(entriesIt.second->get(i)->fields())) {
                value[fieldIt.first] = fieldIt.second;
              }
              entry["values"].append(value);
            }
          }

          tableData["entries"].append(entry);
        }
      }
#endif

            if (!tableData["entries"].empty())
            {
                requestJson["params"]["data"].append(tableData);
            }
        }

        Json::Value responseJson = requestDB(requestJson);

        int code = responseJson["code"].asInt();
        if (code != 0)
        {
            LOG(ERROR) << "Remote database return error:" << code;

            throw StorageException(
                -1, "Remote database return error:" + boost::lexical_cast<std::string>(code));
        }

        int count = responseJson["result"]["count"].asInt();

        return count;
    }
    catch (std::exception& e)
    {
        LOG(ERROR) << "Commit data to database error:" << e.what();

        throw StorageException(-1, std::string("Commit data to database error:") + e.what());
    }

    return 0;
}

bool AMOPStorage::onlyDirty()
{
    return true;
}

Json::Value AMOPStorage::requestDB(const Json::Value& value)
{
    int retry = 0;

    while (true)
    {
        try
        {
            dev::channel::TopicChannelMessage::Ptr request =
                std::make_shared<dev::channel::TopicChannelMessage>();
            request->setType(0x30);
            request->setSeq(_channelRPCServer->newSeq());

            std::stringstream ssOut;
            ssOut << value;

            auto str = ssOut.str();
            LOG(DEBUG) << "Request AMOPDB:" << request->seq() << " " << str;

            request->setTopic(_topic);

            dev::channel::TopicChannelMessage::Ptr response;

            LOG(TRACE) << "Retry Request amdb :" << retry;
            request->setData((const byte*)str.data(), str.size());
            response = _channelRPCServer->pushChannelMessage(request);
            if (response.get() == NULL || response->result() != 0)
            {
                LOG(ERROR) << "requestDB error:" << response->result();

                throw StorageException(
                    -1, "Remote database return error:" +
                            boost::lexical_cast<std::string>(response->result()));
            }

            // 解析topic
            std::string topic = response->topic();
            LOG(DEBUG) << "Receive topic:" << topic;

            std::stringstream ssIn;
            std::string jsonStr(response->data(), response->data() + response->dataSize());
            ssIn << jsonStr;

            LOG(DEBUG) << "AMOPDB Response:" << ssIn.str();

            Json::Value responseJson;
            ssIn >> responseJson;

            auto codeValue = responseJson["code"];
            if (!codeValue.isInt())
            {
                throw StorageException(-1, "undefined amdb error code");
            }

            int code = codeValue.asInt();
            if (code == 1)
            {
                throw StorageException(
                    1, "amdb sql error:" + boost::lexical_cast<std::string>(code));
            }
            if (code != 0 && code != 1)
            {
                throw StorageException(
                    -1, "amdb code error:" + boost::lexical_cast<std::string>(code));
            }

            return responseJson;
        }
        catch (dev::channel::ChannelException& e)
        {
            LOG(ERROR) << "AMDB error: " << e.what();
            LOG(ERROR) << "Retrying...";
        }
        catch (StorageException& e)
        {
            if (e.errorCode() == -1)
            {
                LOG(ERROR) << "AMDB error: " << e.what();
                LOG(ERROR) << "Retrying...";
            }
            else
            {
                throw e;
            }
        }

        ++retry;
        if (_maxRetry != 0 && retry >= _maxRetry)
        {
            LOG(ERROR) << "Reach max retry:" << retry;

            //存储无法正常使用，退出程序
            auto e = StorageException(-1, "Reach max retry");

            _fatalHandler(e);

            BOOST_THROW_EXCEPTION(e);
        }

        sleep(1);
    }
}

void AMOPStorage::setTopic(const std::string& topic)
{
    _topic = topic;
}

#if 0
void AMOPStorage::setBlockHash(h256 blockHash) {
	_blockHash = blockHash;
}

void AMOPStorage::setNum(int num) {
	_num = num;
}
#endif

void AMOPStorage::setChannelRPCServer(dev::ChannelRPCServer::Ptr channelRPCServer)
{
    _channelRPCServer = channelRPCServer;
}

void AMOPStorage::setMaxRetry(int maxRetry)
{
    _maxRetry = maxRetry;
}
