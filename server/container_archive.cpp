﻿/*
	Copyright (c) 2017 TOSHIBA Digital Solutions Corporation

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*!
	@file
	@brief Implementation of Container base class
*/
#include "collection.h"
#include "time_series.h"
#include "gis_geometry.h"
#include "rtree_map.h"
#include "util/trace.h"
#include "btree_map.h"
#include "data_store_v4.h"
#include "data_store_common.h"
#include "transaction_context.h"
#include "gs_error.h"
#include "message_schema.h"
#include "result_set.h"
#include "value_processor.h"

#include "picojson.h"

#include "value.h"

void BaseContainer::getErasableList(TransactionContext &txn, Timestamp erasableTimeUpperLimit, util::XArray<ArchiveInfo> &list) {
	Timestamp erasableTimeLowerLimit = getDataStore()->stats().getBatchFreeTime();
	if (erasableTimeLowerLimit < dataStore_->getConfig().getErasableExpiredTime()) {
		erasableTimeLowerLimit = dataStore_->getConfig().getErasableExpiredTime();
	}
	ExpireType type = getExpireType();
	if (type == TABLE_EXPIRE && baseContainerImage_->rowIdMapOId_ != UNDEF_OID
		&& baseContainerImage_->mvccMapOId_ != UNDEF_OID) {
		ArchiveInfo info;
		info.rowIdMapOId_ = baseContainerImage_->rowIdMapOId_;
		info.mvccMapOId_ = baseContainerImage_->mvccMapOId_;
		info.start_ = getContainerExpirationStartTime();
		info.end_ = getContainerExpirationEndTime();
		info.expired_ = getContainerExpirationTime();
		info.erasable_ = DataStoreV4::DataAffinityUtils::convertChunkKey2Timestamp(
			calcChunkKey(getContainerExpirationEndTime(), getContainerExpirationDutation()));
		if (erasableTimeLowerLimit >= info.erasable_ || info.erasable_ > erasableTimeUpperLimit) {
			return;
		}
		list.push_back(info);
	}
}




std::string BaseContainer::dump(TransactionContext &txn) {
	switch (getContainerType()) {
	case COLLECTION_CONTAINER:
		return dumpImpl<Collection>(txn);
		break;
	case TIME_SERIES_CONTAINER:
		return dumpImpl<TimeSeries>(txn);
		break;
	default:
		GS_THROW_SYSTEM_ERROR(GS_ERROR_DS_CONTAINER_TYPE_UNKNOWN, "");
		break;
	}
	return "";
}

template <typename R>
std::string BaseContainer::dumpImpl(TransactionContext &txn) {
	util::NormalOStringStream strstrm;
	if (isNullsStatsStatus()) {
		strstrm << "Nulls Stats LATEST" << std::endl;
	} else {
		strstrm << "Nulls Stats OLD" << std::endl;
	}

	RowArray rowArray(txn, this);
	util::XArray<OId>::iterator itr;
	strstrm << "==========RowIdMap Map==========" << std::endl;
	BtreeMap::BtreeCursor btreeCursor;
	while (1) {
		util::StackAllocator::Scope scope(txn.getDefaultAllocator());
		util::XArray<OId> oIdList(txn.getDefaultAllocator());
		StackAllocAutoPtr<BtreeMap> rowIdMap(
			txn.getDefaultAllocator(), getRowIdMap(txn));
		int32_t getAllStatus = rowIdMap.get()->getAll(
			txn, PARTIAL_RESULT_SIZE, oIdList, btreeCursor);
		for (itr = oIdList.begin(); itr != oIdList.end(); itr++) {
			rowArray.load(txn, *itr, this, OBJECT_READ_ONLY);
			strstrm << rowArray.dump(txn);
		}
		if (getAllStatus == GS_SUCCESS) {
			break;
		}
	}
	strstrm << "==========Mvcc Map==========" << std::endl;
	while (1) {
		util::StackAllocator::Scope scope(txn.getDefaultAllocator());
		util::XArray<std::pair<TransactionId, MvccRowImage> > idList(
			txn.getDefaultAllocator());
		util::XArray<std::pair<TransactionId, MvccRowImage> >::iterator itr;
		StackAllocAutoPtr<BtreeMap> mvccMap(
			txn.getDefaultAllocator(), getMvccMap(txn));
		int32_t getAllStatus =
			mvccMap.get()->getAll<TransactionId, MvccRowImage>(
				txn, MAX_RESULT_SIZE, idList, btreeCursor);
		for (itr = idList.begin(); itr != idList.end(); itr++) {
			switch (itr->second.type_) {
			case MVCC_SELECT:
				{
					strstrm << "(" << MvccRowImage::getTypeStr(itr->second.type_)
							<< ")" << std::endl;
				}
				break;
			case MVCC_CREATE:
				{
					strstrm << "(" << MvccRowImage::getTypeStr(itr->second.type_)
							<< "), firstRowId = " << itr->second.firstCreateRowId_
							<< ", lastRowId = " << itr->second.lastCreateRowId_
							<< std::endl;
				}
				break;
			case MVCC_INDEX:
				{
					strstrm << "(" << MvccRowImage::getTypeStr(itr->second.type_)
							<< "), mapType = " << (int)itr->second.mapType_
							<< ", columnId = " << itr->second.columnId_
							<< std::endl;
				}
				break;
			case MVCC_CONTAINER:
				{
					strstrm << "(" << MvccRowImage::getTypeStr(itr->second.type_)
							<< "), cursorRowId = " << itr->second.cursor_
							<< ", containerOId = " << itr->second.containerOId_
							<< std::endl;
				}
				break;
			case MVCC_UPDATE:
			case MVCC_DELETE:
				{
					rowArray.load(txn, itr->second.snapshotRowOId_,
						this, OBJECT_READ_ONLY);
					strstrm << "(" << MvccRowImage::getTypeStr(itr->second.type_)
							<< ")," << rowArray.dump(txn);
				}
				break;
			default:
				GS_THROW_SYSTEM_ERROR(GS_ERROR_DS_TYPE_INVALID, "");
				break;
			}
		}
		if (getAllStatus == GS_SUCCESS) {
			break;
		}
	}
	return strstrm.str();
}

std::string BaseContainer::dump(
	TransactionContext &txn, util::XArray<OId> &oIdList) {
	util::NormalOStringStream strstrm;
	strstrm << "==========Row List==========" << std::endl;
	util::XArray<OId>::iterator itr;

	RowArray rowArray(txn, this);
	for (itr = oIdList.begin(); itr != oIdList.end(); itr++) {
		rowArray.load(txn, *itr, this, OBJECT_READ_ONLY);
		RowArray::Row row(rowArray.getRow(), &rowArray);
		strstrm << row.dump(txn) << std::endl;
	}
	return strstrm.str();
}

/*!
	@brief Validates Rows and Indexes
*/
bool BaseContainer::validate(
	TransactionContext &txn, std::string &errorMessage) {
	bool isValid = true;
	RowId preRowId = -1;  
	uint64_t countRowNum = 0;
	switch (getContainerType()) {
	case COLLECTION_CONTAINER:
		isValid =
			validateImpl<Collection>(txn, errorMessage, preRowId, countRowNum);
		break;
	case TIME_SERIES_CONTAINER:
		isValid =
			validateImpl<TimeSeries>(txn, errorMessage, preRowId, countRowNum);
		break;
	default:
		GS_THROW_SYSTEM_ERROR(GS_ERROR_DS_CONTAINER_TYPE_UNKNOWN, "");
		break;
	}
	return isValid;
}

template <typename R>
bool BaseContainer::validateImpl(TransactionContext &txn,
	std::string &errorMessage, RowId &preRowId, uint64_t &countRowNum,
	bool isCheckRowRate) {
	bool isValid = true;
	RowId preRowArrayId = -1;
	uint64_t totalMaxRowNum = 0, totalRowNum = 0, lastMaxRowNum = 0,
			 lastRowNum = 0, firstMaxRowNum = 0, firstRowNum = 0;
	util::NormalOStringStream strstrm;

	RowArray rowArray(txn, reinterpret_cast<R *>(this));
	ContainerId containerId = getContainerId();

	std::map<RowId, uint64_t> rowNumMap;
	std::map<RowId, uint64_t>::iterator rowNumMapItr;
	{
		BtreeMap::BtreeCursor btreeCursor;
		while (1) {
			util::StackAllocator::Scope scope(txn.getDefaultAllocator());
			util::XArray<std::pair<TransactionId, MvccRowImage> > idList(
				txn.getDefaultAllocator());
			util::XArray<std::pair<TransactionId, MvccRowImage> >::iterator itr;
			StackAllocAutoPtr<BtreeMap> mvccMap(
				txn.getDefaultAllocator(), getMvccMap(txn));
			int32_t getAllStatus =
				mvccMap.get()->getAll<TransactionId, MvccRowImage>(
					txn, MAX_RESULT_SIZE, idList, btreeCursor);
			for (itr = idList.begin(); itr != idList.end(); itr++) {
				switch (itr->second.type_) {
				case MVCC_SELECT:
				case MVCC_CREATE:
					break;
				case MVCC_INDEX:
					{
						IndexData indexData(txn.getDefaultAllocator());
						IndexCursor indexCursor = IndexCursor(itr->second);
						bool isExist = getIndexData(txn, indexCursor, indexData);
						if (!isExist) {
							strstrm << (isValid ? ", " : "")
									<< "\"invalidMvccIndexNotExist"
									<< itr->second.snapshotRowOId_ << "\"";
							isValid = false;
							break;
						}
						if (indexData.status_ == DDL_READY) {
							strstrm << (isValid ? ", " : "")
									<< "\"invalidMvccIndexStatusReady"
									<< itr->second.snapshotRowOId_ << "\"";
							isValid = false;
							break;
						}
						rowNumMap.insert(std::make_pair(indexData.cursor_, MAX_RESULT_SIZE));
					}
					break;
				case MVCC_CONTAINER:
					if (!isAlterContainer()) {
						strstrm << (isValid ? ", " : "")
								<< "\"invalidMvccConainerNotLocked\"";
						isValid = false;
					}
					break;
				case MVCC_UPDATE:
				case MVCC_DELETE:
					{
						try {
							rowArray.load(txn, itr->second.snapshotRowOId_,
								reinterpret_cast<R *>(this), OBJECT_READ_ONLY);
							if (!rowArray.validate()) {
								strstrm << (isValid ? ", " : "")
										<< "\"invalidMvccRowArrayOId"
										<< itr->second.snapshotRowOId_ << "\"";
								isValid = false;
								continue;
							}
						}
						catch (std::exception &) {
							strstrm << (isValid ? ", " : "")
									<< "\"invalidMvccRowArrayOId"
									<< itr->second.snapshotRowOId_ << "\"";
							isValid = false;
							continue;
						}

						if (rowArray.getContainerId() != containerId) {
							isValid = false;
							continue;
						}

						RowId rowArrayId = rowArray.getRowId();
						for (rowArray.begin(); !rowArray.end(); rowArray.next()) {
							RowArray::Row row(
								rowArray.getRow(), &rowArray);
							RowId rowId = row.getRowId();
							if (row.isRemoved()) {
								strstrm << (isValid ? ", " : "")
										<< "\"invalidMvccRemovedRow\":\"rowId_"
										<< rowId << "\"";
								isValid = false;
								continue;
							}
							try {
								util::StackAllocator::Scope scope(
									txn.getDefaultAllocator());
								std::string dumpStr = row.dump(txn);
							}
							catch (std::exception &) {
								strstrm << (isValid ? ", " : "")
										<< "\"invalidMvccVariable\":\"rowId_"
										<< rowId << "\"";
								isValid = false;
								continue;
							}
							if (rowId < rowArrayId) {
								isValid = false;
								strstrm << "inValid Mvcc RowId: rowArrayId = "
										<< rowArrayId << ">= rowId = " << rowId
										<< std::endl;
								continue;
							}
						}
						if (!isValid) {
							break;
						}
					}
					break;
				default:
					GS_THROW_SYSTEM_ERROR(GS_ERROR_DS_TYPE_INVALID, "");
					break;
				}
				if (!isValid) {
					break;
				}
			}
			if (getAllStatus == GS_SUCCESS) {
				break;
			}
		}
	}

	util::XArray<IndexData> indexList(txn.getDefaultAllocator());
	try {
		bool withUncommitted = true;
		getIndexList(txn, withUncommitted, indexList);
	}
	catch (std::exception &) {
		strstrm << (isValid ? ", " : "") << "\"invalidIndexSchema\"";
		isValid = false;
	}

	BtreeMap::BtreeCursor btreeCursor;
	while (1) {
		util::StackAllocator::Scope scope(txn.getDefaultAllocator());
		util::XArray<OId> oIdList(txn.getDefaultAllocator());
		util::XArray<OId>::iterator itr;
		StackAllocAutoPtr<BtreeMap> rowIdMap(
			txn.getDefaultAllocator(), getRowIdMap(txn));
		int32_t getAllStatus;
		try {
			getAllStatus = rowIdMap.get()->getAll(
				txn, PARTIAL_RESULT_SIZE, oIdList, btreeCursor);
		}
		catch (std::exception &) {
			strstrm << (isValid ? ", " : "") << "\"invalidRowMapGetAll\"";
			isValid = false;
			break;
		}
		for (size_t i = 0; i < oIdList.size(); i++) {
			try {
				rowArray.load(txn, oIdList[i], reinterpret_cast<R *>(this),
					OBJECT_READ_ONLY);
				if (!rowArray.validate()) {
					strstrm << (isValid ? ", " : "") << "\"invalidRowArrayOId"
							<< oIdList[i] << "\"";
					isValid = false;
					continue;
				}
			}
			catch (std::exception &) {
				strstrm << (isValid ? ", " : "") << "\"invalidRowArrayOId"
						<< oIdList[i] << "\"";
				isValid = false;
				continue;
			}
			if (isCheckRowRate) {
				totalMaxRowNum += rowArray.getMaxRowNum();
				totalRowNum += rowArray.getActiveRowNum();
				lastMaxRowNum = rowArray.getMaxRowNum();
				lastRowNum = rowArray.getActiveRowNum();
				if (firstMaxRowNum == 0) {
					firstMaxRowNum = rowArray.getMaxRowNum();
					firstRowNum = rowArray.getActiveRowNum();
				}
			}

			RowId rowArrayId = rowArray.getRowId();
			if (preRowArrayId >= rowArrayId) {
				isValid = false;
				strstrm << "inValid RowArrayId: preRowArrayId = "
						<< preRowArrayId << ">= rowArrayId = " << rowArrayId
						<< std::endl;
				continue;
			}
			if (rowArray.getContainerId() != containerId) {
				isValid = false;
				continue;
			}

			preRowId = rowArrayId - 1;
			for (rowArray.begin(); !rowArray.end(); rowArray.next()) {
				RowArray::Row row(rowArray.getRow(), &rowArray);
				RowId rowId = row.getRowId();
				if (row.isRemoved()) {
					strstrm << (isValid ? ", " : "")
							<< "\"invalidRemovedRow\":\"rowId_" << rowId
							<< "\"";
					isValid = false;
					continue;
				}
				try {
					util::StackAllocator::Scope scope(
						txn.getDefaultAllocator());
					std::string dumpStr = row.dump(txn);
				}
				catch (std::exception &) {
					strstrm << (isValid ? ", " : "")
							<< "\"invalidVariable\":\"rowId_" << rowId << "\"";
					isValid = false;
					continue;
				}
				if (preRowId >= rowId) {
					isValid = false;
					strstrm << "inValid RowId: preRowId = " << preRowId
							<< ">= rowId = " << rowId << std::endl;
					continue;
				}
				if (rowId < rowArrayId) {
					isValid = false;
					strstrm << "inValid RowId: rowArrayId = " << rowArrayId
							<< ">= rowId = " << rowId << std::endl;
					continue;
				}
				preRowId = rowId;
				countRowNum++;
				rowNumMapItr = rowNumMap.find(rowId);
				if (rowNumMapItr != rowNumMap.end()) {
					rowNumMapItr->second = countRowNum;
				}
			}
			preRowArrayId = rowArrayId;
			if (!isValid) {
				break;
			}
		}
		if (getAllStatus == GS_SUCCESS) {
			break;
		}
		if (!isValid) {
			break;
		}
	}

	if (!indexList.empty()) {
		for (size_t i = 0; i < indexList.size(); i++) {
			uint64_t valIndexRowNum = 0;
			uint64_t valIndexRowNum2 = 0;
			{
			BtreeMap::BtreeCursor btreeCursor;
			RtreeMap::RtreeCursor rtreeCursor;
			while (1) {
				util::StackAllocator::Scope scope(txn.getDefaultAllocator());
				util::XArray<OId> oIdList(txn.getDefaultAllocator());				
				int32_t getAllStatus = GS_SUCCESS;
				try {
					StackAllocAutoPtr<BaseIndex> map(
						txn.getDefaultAllocator(),
						getIndex(txn, indexList[i]));
					if (map.get() == NULL) {
						strstrm << (isValid ? ", " : "")
								<< "{\"invalidGetIndex\",\"type\":"
								<< (int32_t)indexList[i].mapType_
								<< ",\"first columnId\":"
								<< indexList[i].columnIds_->at(0)
								<< ",\"cursor\":"
								<< (int32_t)indexList[i].cursor_ << "}";
						isValid = false;
						continue;
					}
					switch (indexList[i].mapType_) {
					case MAP_TYPE_BTREE:
						getAllStatus =
							reinterpret_cast<BtreeMap *>(map.get())->getAll(
								txn, PARTIAL_RESULT_SIZE, oIdList, btreeCursor);
						break;
					case MAP_TYPE_SPATIAL:
						getAllStatus =
							reinterpret_cast<RtreeMap *>(map.get())->getAll(
								txn, PARTIAL_RESULT_SIZE, oIdList, rtreeCursor);
						break;
					}
				}
				catch (std::exception &) {
					strstrm
						<< (isValid ? ", " : "")
						<< "{\"invalidIndex\",\"type\":"
						<< (int32_t)indexList[i].mapType_
						<< ",\"first columnId\":" << indexList[i].columnIds_->at(0)
						<< ",\"cursor\":" << (int32_t)indexList[i].cursor_
						<< "}";
					isValid = false;
					continue;
				}
				for (util::XArray<OId>::iterator oIdItr = oIdList.begin(); 
					oIdItr != oIdList.end(); oIdItr++) {
					try {
						rowArray.load(txn, *oIdItr,
							reinterpret_cast<R *>(this), OBJECT_READ_ONLY);
					}
					catch (std::exception &) {
						strstrm << (isValid ? ", " : "")
								<< "\"invalidRowArrayOId" << *oIdItr << "\"";
						isValid = false;
						continue;
					}

					RowArray::Row row(rowArray.getRow(), &rowArray);
					RowId rowId = row.getRowId();
					if (row.isRemoved()) {
						strstrm << (isValid ? ", " : "")
								<< "\"invalidRemovedRow\":\"rowId" << rowId
								<< "\"";
						isValid = false;
						continue;
					}
					try {
						util::StackAllocator::Scope scope(
							txn.getDefaultAllocator());
						std::string dumpStr = row.dump(txn);
					}
					catch (std::exception &) {
						strstrm << (isValid ? ", " : "")
								<< "\"invalidVariable\":\"rowId" << rowId
								<< "\"";
						isValid = false;
						continue;
					}
					if (indexList[i].status_ != DDL_READY && rowId > indexList[i].cursor_) {
						strstrm << (isValid ? ", " : "")
							<< "{\"invalidIndexCursor\":\"cursor" << indexList[i].cursor_
								<< "\""
								<< ", \"invalidIndexRowId\":\"rowId" << rowId
								<< "\"}";
						isValid = false;
						continue;
					}
					valIndexRowNum++;
				}
				if (getAllStatus == GS_SUCCESS) {
					break;
				}
			}
			}
			{
			BtreeMap::BtreeCursor btreeCursor;
			RtreeMap::RtreeCursor rtreeCursor;
			while (1) {
				util::StackAllocator::Scope scope(txn.getDefaultAllocator());
				util::XArray<OId> oIdList(txn.getDefaultAllocator());				
				int32_t getAllStatus = GS_SUCCESS;
				try {
					StackAllocAutoPtr<BaseIndex> map(
						txn.getDefaultAllocator(),
						getIndex(txn, indexList[i], true));
					if (map.get() == NULL) {
						break;
					}
					switch (indexList[i].mapType_) {
					case MAP_TYPE_BTREE:
						getAllStatus =
							reinterpret_cast<BtreeMap *>(map.get())->getAll(
								txn, PARTIAL_RESULT_SIZE, oIdList, btreeCursor);
						break;
					case MAP_TYPE_SPATIAL:
						getAllStatus =
							reinterpret_cast<RtreeMap *>(map.get())->getAll(
								txn, PARTIAL_RESULT_SIZE, oIdList, rtreeCursor);
						break;
					}
				}
				catch (std::exception &) {
					strstrm
						<< (isValid ? ", " : "")
						<< "{\"invalidIndex\",\"type\":"
						<< (int32_t)indexList[i].mapType_
						<< ",\"first columnId\":" << indexList[i].columnIds_->at(0)
						<< ",\"cursor\":" << (int32_t)indexList[i].cursor_
						<< "}";
					isValid = false;
					continue;
				}
				for (util::XArray<OId>::iterator oIdItr = oIdList.begin(); 
					oIdItr != oIdList.end(); oIdItr++) {
					try {
						rowArray.load(txn, *oIdItr,
							reinterpret_cast<R *>(this), OBJECT_READ_ONLY);
					}
					catch (std::exception &) {
						strstrm << (isValid ? ", " : "")
								<< "\"invalidRowArrayOId" << *oIdItr << "\"";
						isValid = false;
						continue;
					}

					RowArray::Row row(rowArray.getRow(), &rowArray);
					RowId rowId = row.getRowId();
					if (row.isRemoved()) {
						strstrm << (isValid ? ", " : "")
								<< "\"invalidRemovedRow\":\"rowId" << rowId
								<< "\"";
						isValid = false;
						continue;
					}
					try {
						util::StackAllocator::Scope scope(
							txn.getDefaultAllocator());
						std::string dumpStr = row.dump(txn);
					}
					catch (std::exception &) {
						strstrm << (isValid ? ", " : "")
								<< "\"invalidVariable\":\"rowId" << rowId
								<< "\"";
						isValid = false;
						continue;
					}
					if (indexList[i].status_ != DDL_READY && rowId > indexList[i].cursor_) {
						strstrm << (isValid ? ", " : "")
							<< "{\"invalidIndexCursor\":\"cursor" << indexList[i].cursor_
								<< "\""
								<< ", \"invalidIndexRowId\":\"rowId" << rowId
								<< "\"}";
						isValid = false;
						continue;
					}
					valIndexRowNum++;
					valIndexRowNum2++;
				}
				if (getAllStatus == GS_SUCCESS) {
					break;
				}
			}
			}
			uint64_t validateRowNum = countRowNum;
			rowNumMapItr = rowNumMap.find(indexList[i].cursor_);
			if (rowNumMapItr != rowNumMap.end()) {
				validateRowNum = rowNumMapItr->second;
			}

			if (isValid) {
				if (indexList[i].status_ == DDL_READY && validateRowNum != valIndexRowNum) {
					strstrm
						<< (isValid ? ", " : "")
						<< "{\"invalidIndexRowNum\",\"type\":"
						<< (int32_t)indexList[i].mapType_
						<< ",\"first columnId\":" << indexList[i].columnIds_->at(0)
						<< ",\"cursor\":" << (int32_t)indexList[i].cursor_
						<< ", \"rowCount\":" << validateRowNum
						<< ", \"indexRowCount\":" << valIndexRowNum << "}";
					isValid = false;
				}
			}
			else {
				strstrm << (isValid ? ", " : "")
						<< "{\"invalidIndexValue\",\"type\":"
						<< (int32_t)indexList[i].mapType_
						<< ",\"first columnId\":" << indexList[i].columnIds_->at(0)
						<< ",\"cursor\":" << (int32_t)indexList[i].cursor_
						<< "}";
				isValid = false;
			}
		}
	}
	if (isValid) {
		if (getContainerType() == COLLECTION_CONTAINER) {
			StackAllocAutoPtr<BtreeMap> mvccMap(
				txn.getDefaultAllocator(), getMvccMap(txn));
			if (mvccMap.get()->isEmpty() && getRowNum() != countRowNum) {
				isValid = false;
				strstrm << "inValid row num: container rowNum(" << getRowNum()
						<< ") != totalRowNum(" << countRowNum << ")"
						<< std::endl;
			}
		}
		if (isCheckRowRate) {
			if (countRowNum > 1) {
				totalMaxRowNum =
					totalMaxRowNum - lastMaxRowNum - firstMaxRowNum;
				totalRowNum = totalRowNum - lastRowNum - firstRowNum;
			}
			else {
				totalMaxRowNum = totalMaxRowNum - lastMaxRowNum;
				totalRowNum = totalRowNum - lastRowNum;
			}
			if (totalMaxRowNum > totalRowNum * 2) {
				isValid = false;
				strstrm << "inValid row rate: totalMaxRowNum(" << totalMaxRowNum
						<< ") > totalRowNum(" << totalRowNum << ") * 2"
						<< std::endl;
			}
		}
	}
	errorMessage = strstrm.str().c_str();
	strstrm << "], \"rowNum\":" << countRowNum << "}";
	if (!isValid) {
		UTIL_TRACE_ERROR(DATA_STORE, strstrm.str());
	}
	return isValid;
}





