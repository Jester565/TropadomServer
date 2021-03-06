#include "TerrainManager.h"
#include "Trop.pb.h"
#include "PerlinManager.h"
#include "CaveManager.h"
#include "TropServer.h"
#include "TropClientManager.h"
#include "TropClient.h"
#include "DepositManager.h"
#include <IPacket.h>
#include <OPacket.h>
#include <fstream>
#include <iostream>

TerrainManager::TerrainManager(TropServer* tropServer)
	:PKeyOwner(), tropServer(tropServer)
{
	PKeyOwner::attach(tropServer->getPacketManager());
}

bool TerrainManager::init(int64_t seed)
{
	this->seed = seed;
	return init();
}

bool TerrainManager::init(const std::string & saveFileName)
{
	if (load(saveFileName))
	{
		return init();
	}
	return false;
}


void TerrainManager::merge(std::list<TerrainSection>::iterator iter1, std::list<TerrainSection>::iterator iter2)
{
	iter1->blockColumns.splice(iter1->blockColumns.end(), iter2->blockColumns);
	boost::static_pointer_cast<TropClientManager>(tropServer->getClientManager())->replaceTerrainSection(iter2, iter1);
	terrainSections.erase(iter2);
}

void TerrainManager::shiftLeft(TerrainTracker * terTrack)
{
	//read lock terTrack->tsIter
	//read lock (upgradable) terTrack->tsIter->blockColumns
	terTrack->posMutex.lock();
	boost::upgrade_lock <boost::shared_mutex> bc1ULock(terTrack->tsIter->blockColumnsMutex);
	if (terTrack->bcIterLow->bX - 1 < terTrack->tsIter->blockColumns.front().bX)
	{  
		bool merged = false;
		//read lock (upgradable) terrainSections
		boost::upgrade_lock <boost::shared_mutex> terrainSectionsULock(terrainSectionsMutex);
		if (terTrack->tsIter != terrainSections.begin())
		{
			auto lowTSIter = terTrack->tsIter;
			lowTSIter--;
			//read lock (upgradable) lowTSITer->blockColumns
			boost::upgrade_lock <boost::shared_mutex> bc2ULock(lowTSIter->blockColumnsMutex);
			if (lowTSIter->blockColumns.back().bX == terTrack->bcIterLow->bX - 1)
			{
				//upgrade lock iter1->blockColumns
				//upgrade lock iter2->blockColumns
				//upgrade lock to terrainSections
				boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueBC1Lock(bc1ULock);
				boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueBC2Lock(bc2ULock);
				boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueTerrainSectionsLock(terrainSectionsULock);
				merge(lowTSIter, terTrack->tsIter);
				merged = true;
			}
		}
		if (!merged)
		{
			//upgrade lock terTrack->tsIter->blockColumns
			boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueBC1Lock(bc1ULock);
			terTrack->tsIter->blockColumns.emplace_front(worldGenerator, terTrack->bcIterLow->bX - 1);
		}
	}
	terTrack->posMutex.unlock();
	terTrack->bcIterLow--;
	terTrack->bcIterHigh--;
}

void TerrainManager::shiftRight(TerrainTracker * terTrack)
{
	//read lock terTrack->tsIter
	//read lock terTrack->tsIter->blockColumn
	terTrack->posMutex.lock();
	boost::upgrade_lock <boost::shared_mutex> bc1ULock(terTrack->tsIter->blockColumnsMutex);
	if (terTrack->bcIterHigh->bX + 1 > terTrack->tsIter->blockColumns.back().bX)
	{
		bool merged = false;
		//read lock terrainSections
		boost::upgrade_lock <boost::shared_mutex> terrainSectionsULock(terrainSectionsMutex);
		auto endIter = terrainSections.end();
		endIter--;
		if (terTrack->tsIter != endIter)
		{
			auto highTSIter = terTrack->tsIter;
			highTSIter++;
			//read lock highTSIter->blockColumns
			boost::upgrade_lock <boost::shared_mutex> bc2ULock(highTSIter->blockColumnsMutex);
			if (highTSIter->blockColumns.front().bX == terTrack->bcIterHigh->bX + 1)
			{
				//upgrade lock iter1->blockColumns
				//upgrade lock iter2->blockColumns
				//upgrade lock to terrainSections
				boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueBC1Lock(bc1ULock);
				boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueBC2Lock(bc2ULock);
				boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueTerrainSectionsLock(terrainSectionsULock);
				if (terTrack->bcIterHigh->bX + 1 > terTrack->tsIter->blockColumns.back().bX && terTrack->tsIter != endIter)
				{
					merge(terTrack->tsIter, highTSIter);
					merged = true;
				}
			}
		}
		if (!merged)
		{
			//write lock terTrack->tsIter->blockColumns
			boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueBC1Lock(bc1ULock);
			terTrack->tsIter->blockColumns.emplace_back(worldGenerator, terTrack->bcIterHigh->bX + 1);
		}
	}
	terTrack->posMutex.unlock();
	terTrack->bcIterHigh++;
	terTrack->bcIterLow++;
}

void TerrainManager::teleLoad(int64_t bX, TerrainTracker * terTrack)
{
	terTrack->posMutex.lock();
	//read lock (upgradable) terrainSections
	boost::upgrade_lock <boost::shared_mutex> terrainSectionsULock(terrainSectionsMutex);
	for (auto it = terrainSections.begin(); it != terrainSections.end(); it++)
	{
		//read lock it->blockColumns
		boost::shared_lock <boost::shared_mutex> bcLock(it->blockColumnsMutex);
		if (bX >= it->blockColumns.front().bX && bX <= it->blockColumns.back().bX)
		{
			auto bcIter = it->blockColumns.begin();
			for (int cBX = it->blockColumns.front().bX; cBX < bX; cBX++)
			{
				bcIter++;
			}
			terTrack->bcIterHigh = bcIter;
			terTrack->bcIterLow = bcIter;
			//write lock terTrack->tsIter
			terTrack->tsIter = it;
			terTrack->posMutex.unlock();
			return;
		}
		else if (bX < it->blockColumns.front().bX)
		{
			//write lock terrainSections
			boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueTerrainSectionsLock(terrainSectionsULock);
			auto empIter = terrainSections.emplace(it, worldGenerator, bX);
			terTrack->bcIterHigh = empIter->blockColumns.begin();
			terTrack->bcIterLow = empIter->blockColumns.begin();
			//write lock terTrack->tsIter
			terTrack->tsIter = empIter;
			terTrack->posMutex.unlock();
			return;
		}
	}
	boost::upgrade_to_unique_lock <boost::shared_mutex> uniqueTerrainSectionsLock(terrainSectionsULock);
	terrainSections.emplace_back(worldGenerator, bX);
	terTrack->bcIterHigh = terrainSections.back().blockColumns.begin();
	terTrack->bcIterLow = terrainSections.back().blockColumns.begin();
	//write lock terTrack->tsIter
	terTrack->tsIter = terrainSections.end();
	terTrack->tsIter--;
	terTrack->posMutex.unlock();
}

bool TerrainManager::save(const std::string& saveFileName)
{
	union {
		uint32_t i;
		char c[4];
	} tbend = { 0x01020304 };
	bool bEndian = (tbend.c[0] == 1);
	std::ofstream saveFile;
	saveFile.open(saveFileName);
	std::vector <char> seedData(8);
	if (!bEndian)
	{
		for (int i = 0; i < 8; i++)
		{
			seedData.at(7 - i) = (seed >> (i * 8)) & 0xff;
		}
	}
	else
	{
		for (int i = 0; i < 8; i++)
		{
			seedData.at(i) = (seed >> (i * 8)) & 0xff;
		}
	}
	std::cout << seed << std::endl;
	saveFile.write(seedData.data(), 8);
	boost::shared_lock <boost::shared_mutex>(terrainSectionsMutex);
	for (auto tsIt = terrainSections.begin(); tsIt != terrainSections.end(); tsIt++)
	{
		TropPackets::PackE0 savePack;
		boost::shared_lock <boost::shared_mutex>(tsIt->blockColumnsMutex);
		tsIt->blockColumns.begin()->blocksMutex.lock_shared();
		savePack.set_bx(tsIt->blockColumns.begin()->bX);
		tsIt->blockColumns.begin()->blocksMutex.unlock_shared();
		for (auto bcIt = tsIt->blockColumns.begin(); bcIt != tsIt->blockColumns.end(); bcIt++)
		{
			bcIt->blocksMutex.lock_shared();
			savePack.add_blocktypes(bcIt->groundY);
			for (int i = 0; i < COL_HEIGHT; i++)
			{
				savePack.add_blocktypes(bcIt->blocks.at(i));
			}
			bcIt->blocksMutex.unlock_shared();
		}
		uint32_t byteSize = savePack.ByteSize();
		std::vector <char> sizeData(4);
		if (!bEndian)
		{
			sizeData.at(3) = (byteSize & 0xff);
			sizeData.at(2) = (byteSize >> 8) & 0xff;
			sizeData.at(1) = (byteSize >> 16) & 0xff;
			sizeData.at(0) = (byteSize >> 24) & 0xff;
		}
		else
		{
			sizeData.at(0) = (byteSize & 0xff);
			sizeData.at(1) = (byteSize >> 8) & 0xff;
			sizeData.at(2) = (byteSize >> 16) & 0xff;
			sizeData.at(3) = (byteSize >> 24) & 0xff;
		}
		saveFile.write(sizeData.data(), sizeData.size());
		savePack.SerializeToOstream(&saveFile);
	}
	std::cout << "SAVED" << std::endl;
	saveFile.close();
	return true;
}

bool TerrainManager::load(const std::string& saveFileName)
{
	union {
		uint32_t i;
		char c[4];
	} tbend = { 0x01020304 };
	bool bEndian = (tbend.c[0] == 1);
	std::vector <char> seedData(8);
	std::ifstream saveFile;
	saveFile.open(saveFileName);
	if (!saveFile.is_open())
	{
		return false;
	}
	seed = 0;
	saveFile.read(seedData.data(), 8);
	if (!bEndian)
	{
		for (int i = 0; i < 8; i++)
		{
			seed |= ((uint64_t)(seedData.at(7 - i) & 0xff)) << (i * 8);
		}
	}
	else
	{
		for (int i = 0; i < 8; i++)
		{
			seed |= ((uint64_t)(seedData.at(i) & 0xff)) << (i * 8);
		}
	}
	std::cout << "LOADING WORLD: " << seed << std::endl;
	while (true)
	{
		std::vector <char> sizeData(4);
		saveFile.read(sizeData.data(), 4);
		if (saveFile.eof())
		{
			break;
		}
		uint32_t packSize = 0;
		if (!bEndian)
		{
			packSize = ((sizeData.at(0) & 0xff) << 24) | ((sizeData.at(1) & 0xff) << 16) |
				((sizeData.at(2) & 0xff) << 8) | (sizeData.at(3) & 0xff);
		}
		else
		{
			packSize = ((sizeData.at(3) & 0xff) << 24) | ((sizeData.at(2) & 0xff) << 16) |
				((sizeData.at(1) & 0xff) << 8) | (sizeData.at(0) & 0xff);
		}
		std::vector <char> packData (packSize);
		saveFile.read(packData.data(), packSize);
		TropPackets::PackE0 savePack;
		savePack.ParseFromArray(packData.data(), packSize);
		int64_t bX = savePack.bx();
		terrainSections.emplace_back();
		auto tsIt = terrainSections.end();
		tsIt--;
		for (int i = 0; i < savePack.blocktypes_size(); i+=COL_HEIGHT + 1)
		{
			tsIt->blockColumns.emplace_back(bX, savePack.blocktypes().Get(i));
			auto bcIt = tsIt->blockColumns.end();
			bcIt--; 
			for (int j = 1; j < COL_HEIGHT + 1; j++)
			{
				bcIt->blocks.at(j - 1) = savePack.blocktypes().Get(i + j);
			}
			bX++;
		}
	}
	saveFile.close();
	return true;
}

TerrainManager::~TerrainManager()
{
	delete worldGenerator;
	worldGenerator = nullptr;
}

bool TerrainManager::init()
{
	worldGenerator = new WorldGenerator(seed);
	addKey(boost::make_shared<PKey>("C0", this, &TerrainManager::keyC0));
	addKey(boost::make_shared<PKey>("C2", this, &TerrainManager::keyC2));
	addKey(boost::make_shared<PKey>("D0", this, &TerrainManager::keyD0));
	addKey(boost::make_shared<PKey>("G0", this, &TerrainManager::keyG0));
	return true;
}

void TerrainManager::keyC0(boost::shared_ptr<IPacket> iPack)
{
	auto sender = boost::static_pointer_cast<TropClient>(iPack->getSender());
	TropPackets::PackC0 packC0;
	packC0.ParseFromString(*iPack->getData());
	int64_t bX = packC0.bx();
	int64_t bY = packC0.by();
	teleLoad(bX, sender->getTerrainTracker());
	TropPackets::PackC1 packC1;
	packC1.mutable_blockarr()->Reserve(LOAD_COL_W * LOAD_COL_H);
	sender->getTerrainTracker()->posMutex.lock();
	for (int i = 0; i < LOAD_COL_W; i++)
	{
		sender->getTerrainTracker()->bcIterHigh->blocksMutex.lock_shared();
		packC1.add_groundys(sender->getTerrainTracker()->bcIterHigh->groundY);
		for (int j = 0; j < LOAD_COL_H; j++)
		{
		 	if (bY + j < 0 || bY + j >= COL_HEIGHT)
			{
				packC1.add_blockarr(0);
			}
			else
			{
				packC1.add_blockarr(sender->getTerrainTracker()->bcIterHigh->blocks.at(bY + j));
			}
		}
		sender->getTerrainTracker()->bcIterHigh->blocksMutex.unlock_shared();
		if (i != LOAD_COL_W - 1)
		{
			shiftRight(sender->getTerrainTracker());
			sender->getTerrainTracker()->bcIterLow--;
		}
	}
	sender->getTerrainTracker()->rowILow = bY;
	sender->getTerrainTracker()->rowIHigh = bY + LOAD_COL_H - 1;
	sender->getTerrainTracker()->posMutex.unlock();
	boost::shared_ptr <OPacket> opC1(new OPacket("C1", 0));
	opC1->setData(boost::make_shared <std::string>(packC1.SerializeAsString()));
	opC1->addSendToID(sender->getID());
	tropServer->getClientManager()->send(opC1, sender);
}

void TerrainManager::keyC2(boost::shared_ptr<IPacket> iPack)
{
	c2RecieveCount++;
	auto sender = boost::static_pointer_cast<TropClient>(iPack->getSender());
	TropPackets::PackC2 packC2;
	packC2.ParseFromString(*iPack->getData());
	TropPackets::PackC3 packC3;
	sender->getTerrainTracker()->posMutex.lock();
	if (packC2.horizontal())
	{
		c2HorizontalCount++;
		packC3.set_horizontal(true);
		int64_t bY = sender->getTerrainTracker()->rowILow;
		packC3.set_by(bY);
		packC3.mutable_blockarr()->Reserve(LOAD_COL_H);
		if (packC2.shiftpositive())
		{
			shiftRight(sender->getTerrainTracker());
			packC3.set_bx(sender->getTerrainTracker()->bcIterHigh->bX);
			packC3.set_groundy(sender->getTerrainTracker()->bcIterHigh->groundY);
			sender->getTerrainTracker()->bcIterHigh->blocksMutex.lock_shared();
			for (int j = 0; j < LOAD_COL_H; j++)
			{
				if (bY + j < 0 || bY + j >= COL_HEIGHT)
				{
					packC3.add_blockarr(0);
				}
				else
				{
					packC3.add_blockarr(sender->getTerrainTracker()->bcIterHigh->blocks.at(bY + j));
				}
			}
			sender->getTerrainTracker()->bcIterHigh->blocksMutex.unlock_shared();
		}
		else
		{
			shiftLeft(sender->getTerrainTracker());
			packC3.set_bx(sender->getTerrainTracker()->bcIterLow->bX);
			packC3.set_groundy(sender->getTerrainTracker()->bcIterLow->groundY);
			sender->getTerrainTracker()->bcIterLow->blocksMutex.lock_shared();
			for (int j = 0; j < LOAD_COL_H; j++)
			{
				if (bY + j < 0 || bY + j >= COL_HEIGHT)
				{
					packC3.add_blockarr(0);
				}
				else
				{
					packC3.add_blockarr(sender->getTerrainTracker()->bcIterLow->blocks.at(bY + j));
				}
			}
			sender->getTerrainTracker()->bcIterLow->blocksMutex.unlock_shared();
		}
	}
	else
	{
		packC3.set_horizontal(false);
		packC3.set_bx(sender->getTerrainTracker()->bcIterLow->bX);
		packC3.mutable_blockarr()->Reserve(LOAD_COL_W);
		if (packC2.shiftpositive())
		{
			sender->getTerrainTracker()->rowILow++;
			sender->getTerrainTracker()->rowIHigh++;
			packC3.set_by(sender->getTerrainTracker()->rowIHigh);
			if (sender->getTerrainTracker()->rowIHigh < 0 || sender->getTerrainTracker()->rowIHigh >= COL_HEIGHT)
			{
				for (int i = 0; i < LOAD_COL_W; i++)
				{
					packC3.add_blockarr(0);
				}
			}
			else
			{
				for (auto it = sender->getTerrainTracker()->bcIterLow; it != sender->getTerrainTracker()->bcIterHigh; it++)
				{
					it->blocksMutex.lock_shared();
					packC3.add_blockarr(it->blocks.at(sender->getTerrainTracker()->rowIHigh));
					it->blocksMutex.unlock_shared();
				}
				sender->getTerrainTracker()->bcIterHigh->blocksMutex.lock_shared();
				packC3.add_blockarr(sender->getTerrainTracker()->bcIterHigh->blocks.at(sender->getTerrainTracker()->rowIHigh));
				sender->getTerrainTracker()->bcIterHigh->blocksMutex.unlock_shared();
			}
		}
		else
		{
			sender->getTerrainTracker()->rowILow--;
			sender->getTerrainTracker()->rowIHigh--;
			packC3.set_by(sender->getTerrainTracker()->rowILow);
			if (sender->getTerrainTracker()->rowILow < 0 || sender->getTerrainTracker()->rowILow >= COL_HEIGHT)
			{
				for (int i = 0; i < LOAD_COL_W; i++)
				{
					packC3.add_blockarr(0);
				}
			}
			else
			{
				for (auto it = sender->getTerrainTracker()->bcIterLow; it != sender->getTerrainTracker()->bcIterHigh; it++)
				{
					it->blocksMutex.lock_shared();
					packC3.add_blockarr(it->blocks.at(sender->getTerrainTracker()->rowILow));
					it->blocksMutex.unlock_shared();
				}
				sender->getTerrainTracker()->bcIterHigh->blocksMutex.lock_shared();
				packC3.add_blockarr(sender->getTerrainTracker()->bcIterHigh->blocks.at(sender->getTerrainTracker()->rowILow));
				sender->getTerrainTracker()->bcIterHigh->blocksMutex.unlock_shared();
			}
		}
	}
	sender->getTerrainTracker()->posMutex.unlock();
	boost::shared_ptr <OPacket> opC3(new OPacket("C3", 0));
	opC3->addSendToID(sender->getID());
	opC3->setData(boost::make_shared<std::string>(packC3.SerializeAsString()));
	tropServer->getClientManager()->send(opC3);
} 

void TerrainManager::keyD0(boost::shared_ptr<IPacket> iPack)
{
	auto sender = boost::static_pointer_cast<TropClient>(iPack->getSender());
	TropPackets::PackD0 packD0;
	packD0.ParseFromString(*iPack->getData());
	sender->getTerrainTracker()->posMutex.lock();
	if (packD0.bx() >= sender->getTerrainTracker()->bcIterLow->bX && packD0.bx() <= sender->getTerrainTracker()->bcIterHigh->bX)
	{
		auto it = sender->getTerrainTracker()->bcIterLow;
		for (int i = 0; i < packD0.bx() - sender->getTerrainTracker()->bcIterLow->bX; i++)
		{
			it++;
		}
		it->blocksMutex.lock();
		bool groundHChanged = false;
		it->setBlock(packD0.by(), packD0.type(), &groundHChanged);
		it->blocksMutex.unlock();
		auto tropClientManager = boost::static_pointer_cast<TropClientManager>(tropServer->getClientManager());
		auto inRangeClients = tropClientManager->getInRange(packD0.bx(), packD0.by(), sender);
		if (inRangeClients.size() > 0)
		{
			boost::shared_ptr <OPacket> opD0(new OPacket("D0", sender->getID()));
			for (int i = 0; i < inRangeClients.size(); i++)
			{
				opD0->addSendToID(inRangeClients.at(i)->getID());
			}
			opD0->setData(iPack->getData());
			tropClientManager->send(opD0);
			if (groundHChanged)
			{
				TropPackets::PackD1 packD1;
				packD1.set_bx(packD0.bx());
				packD1.set_groundy(it->groundY);
				boost::shared_ptr <OPacket> opD1(new OPacket("D1", 0));
				opD1->addSendToID(sender->getID());
				for (int i = 0; i < inRangeClients.size(); i++)
				{
					opD1->addSendToID(inRangeClients.at(i)->getID());
				}
				opD1->setData(boost::make_shared <std::string>(packD1.SerializeAsString()));
				tropClientManager->send(opD1);
			}
		}
	}
	sender->getTerrainTracker()->posMutex.unlock();
}

void TerrainManager::keyG0(boost::shared_ptr<IPacket> iPack)
{
	boost::static_pointer_cast<TropClientManager>(tropServer->getClientManager())->sendToAllExceptUDP(iPack->toOPack(true), iPack->getSenderID());
}

BlockColumn::BlockColumn(int64_t bX, uint32_t groundY)
	:bX(bX), groundY(groundY)
{
	blocks.resize(COL_HEIGHT);
}

BlockColumn::BlockColumn(WorldGenerator* worldGen, int64_t bX)
	:bX(bX)
{
	groundY = COL_HEIGHT;
	int iGroundY = worldGen->groundPerlinManager->getPerlinVal(bX, 100, 60);
	int iDirtY = worldGen->dirtPerlinManager->getPerlinVal(bX, 7, 2);
	blocks.reserve(COL_HEIGHT);
	for (int i = 0; i < COL_HEIGHT; i++)
	{
		if (i >= 0 && i < iGroundY)
		{
			blocks.push_back(0);
			continue;
		}
		std::vector <std::pair <int, int>> cavePoints = worldGen->caveManager->genCavePoints(bX);
		bool inCave = false;
		for (int j = 0; j < cavePoints.size(); j++)
		{
			if (cavePoints.at(j).first <= i && cavePoints.at(j).second >= i)
			{
				blocks.push_back(0);
				inCave = true;
				break;
			}
		}
		if (inCave)
		{
			continue;
		}
		if (groundY == COL_HEIGHT)
		{
			groundY = i;
		}
		if (i == iGroundY)
		{
			blocks.push_back(2);
			continue;
		}
		else if (i < iGroundY + iDirtY)
		{
			blocks.push_back(3);
			continue;
		}
		if (worldGen->ironManager->isOre(bX, i))
		{
			blocks.push_back(5);
			continue;
		}
		blocks.push_back(4);
	}
}

void BlockColumn::setBlock(uint32_t bY, uint16_t type, bool* groundHChanged)
{
	if (bY < 0 && bY >= COL_HEIGHT)
	{
		return;
	}
	bool transparent = TransparentTypes.at(type);
	if (bY < groundY)
	{
		if (!transparent)
		{
			groundY = bY;
			*groundHChanged = true;
		}
	}
	else if (bY == groundY)
	{
		if (transparent)
		{
			int i = bY + 1;
			for (; i < COL_HEIGHT; i++)
			{
				if (!TransparentTypes.at(blocks.at(i)))
				{
					break;
				}
			}
			groundY = i;
			*groundHChanged = true;
		}
	}
	blocks.at(bY) = type;
}

BlockColumn::~BlockColumn()
{
}

TerrainSection::TerrainSection()
{
}

TerrainSection::TerrainSection(WorldGenerator* worldGen, int64_t bX)
{
	blockColumns.emplace_back(worldGen, bX);
}

TerrainSection::~TerrainSection()
{
}

WorldGenerator::WorldGenerator(int64_t seed)
{
	groundPerlinManager = new PerlinManager(seed);
	caveManager = new CaveManager(seed * .99, 60, 300);
	dirtPerlinManager = new PerlinManager(seed / 1.3);
	ironManager = new DepositManager(seed, 100, 4, 20, 2);
}

WorldGenerator::~WorldGenerator()
{
	delete groundPerlinManager;
	groundPerlinManager = nullptr;
	delete caveManager;
	caveManager = nullptr;
	delete dirtPerlinManager;
	dirtPerlinManager = nullptr;
	delete ironManager;
	ironManager = nullptr;
}
