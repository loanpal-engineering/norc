/**
 * This file is part of the norc (R) project.
 * Copyright (c) 2017-2018
 * Authors: Cory Mickelson, et al.
 *
 * norc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * norc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "Writer.h"
#include "Internal.h"
#include "MemoryFile.h"
#include "ValidateArguments.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <orc/ColumnPrinter.hh>
#include <sstream>
#include <utility>

#define NAPI_EXPERIMENTAL
#include <node_api.h>

using namespace Napi;
using namespace orc;

using std::cout;
using std::endl;
using std::find;
using std::ifstream;
using std::make_unique;
using std::move;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;

namespace fs = std::filesystem;

namespace norc {
FunctionReference Writer::constructor; // NOLINT

void
Writer::Initialize(Napi::Env& env, Napi::Object& target)
{
  HandleScope scope(env);
  auto ctor = DefineClass(env,
                          "Writer",
                          { InstanceMethod("close", &norc::Writer::Close),
                            InstanceMethod("schema", &norc::Writer::Schema),
                            InstanceMethod("fromCsv", &norc::Writer::ImportCSV),
                            InstanceMethod("add", &norc::Writer::Add),
                            InstanceMethod("data", &norc::Writer::Data),
                            InstanceMethod("merge", &norc::Writer::Merge) });
  constructor = Napi::Persistent(ctor);
  constructor.SuppressDestruct();
  target.Set("Writer", ctor);
}

Writer::Writer(const CallbackInfo& info)
  : ObjectWrap(info)
{
  buffer = make_unique<DataBuffer<char>>(*getDefaultPool(), 4 * 1024 * 1024);
  options.setStripeSize((128 << 20));
  options.setCompressionBlockSize((64 << 10));
  options.setCompression(CompressionKind_ZLIB);
  if (info.Length() == 0) {
    output = make_unique<MemoryWriter>(MEMORY_FILE_SIZE);
    options.setMemoryPool(getDefaultPool());
  } else {
    output = writeLocalFile(info[0].As<String>());
  }
}

void
Writer::Schema(const CallbackInfo& info)
{
  if (info.Length() == 1 && info[0].IsString()) {
    string schema = info[0].As<String>();
    type = Type::buildTypeFromString(schema);
    writer = createWriter(*type, output.get(), options);
    return;
  }
  if (info.Length() < 1 || !info[0].IsObject()) {
    TypeError::New(info.Env(), "The schema as an Object format is required")
      .ThrowAsJavaScriptException();
  }
  stringstream typeStr;
  typeStr << "struct<";
  auto schema = info[0].As<Object>();
  auto keys = schema.GetPropertyNames();
  for (uint32_t i = 0; i < keys.Length(); ++i) {
    string k = keys.Get(i).As<String>();
    TypeKind kind;
    string schemaType;
    JsSchemaDataType jsv =
      static_cast<JsSchemaDataType>(schema.Get(k).As<Number>().Int32Value());
    switch (jsv) {
      case BOOLEAN: {
        kind = TypeKind::BOOLEAN;
        schemaType = "boolean";
        break;
      }
      case TINYINT: {
        kind = TypeKind::BYTE;
        schemaType = "tinyint";
        break;
      }
      case SMALLINT: {
        kind = TypeKind::SHORT;
        schemaType = "smallint";
        break;
      }
      case INT: {
        kind = TypeKind::INT;
        schemaType = "int";
        break;
      }
      case BIGINT: {
        kind = TypeKind::LONG;
        schemaType = "long";
        break;
      }
      case FLOAT: {
        kind = TypeKind::FLOAT;
        schemaType = "float";
        break;
      }
      case DOUBLE: {
        kind = TypeKind::DOUBLE;
        schemaType = "double";
        break;
      }
      case STRING: {
        kind = TypeKind::STRING;
        schemaType = "string";
        break;
      }
      case BINARY: {
        kind = TypeKind::BINARY;
        schemaType = "binary";
        break;
      }
      case TIMESTAMP: {
        kind = TypeKind::TIMESTAMP;
        schemaType = "timestamp";
        break;
      }
      case DECIMAL: {
        kind = TypeKind::DECIMAL;
        schemaType = "decimal";
        break;
      }
      case DATE: {
        kind = TypeKind::DATE;
        schemaType = "date";
        break;
      }
      case CHAR: {
        kind = TypeKind::CHAR;
        schemaType = "char";
        break;
      }
      case VARCHAR: {
        kind = TypeKind::VARCHAR;
        schemaType = "varchar";
        break;
      }
      case ARRAY:
      case MAP:
      case STRUCT:
      case UNION:
      default: {
        Error::New(info.Env(), "Unsupported type").ThrowAsJavaScriptException();
        break;
      }
    }
    this->schema.emplace_back(pair<string, TypeKind>(k, kind));
    typeStr << k << ":" << schemaType;
    if (i != keys.Length() - 1)
      typeStr << ",";
  }
  typeStr << ">";
  cout << "Setting File Schema as: " << typeStr.str() << endl;
  type = Type::buildTypeFromString(typeStr.str());
  writer = createWriter(*type, output.get(), options);
  batch = writer->createRowBatch(batchSize);
}

void
Writer::Add(const CallbackInfo& info)
{
  if (info.Length() > 0 && info[0].IsArray()) {
    auto chunk = info[0].As<Array>();
    for (unsigned int i = 0; i < chunk.Length(); i++) {
      AddObject(info, chunk.Get(static_cast<uint32_t>(i)).As<Object>());
    }
  } else if (info.Length() > 0 && info[0].IsObject()) {
    AddObject(info, info[0].As<Object>());
  }
}
void
Writer::AddObject(const CallbackInfo& info, Object value)
{
  auto properties = value.GetPropertyNames();
  auto row = dynamic_cast<StructVectorBatch*>(batch.get());
  if (properties.Length() != schema.size()) {
    Error::New(info.Env(), "Item does not match schema")
      .ThrowAsJavaScriptException();
    return;
  }
  if (batchOffset == batchSize - 1) {
    row->numElements = batchOffset;
    writer->add(*batch);
    batchOffset = 0;
  }
  for (uint32_t i = 0; i < properties.Length(); i++) {
    string p = properties.Get(i).As<String>();
    unsigned int idx = 0;
    for (; idx < schema.size(); idx++) {
      if (schema[idx].first == p) {
        break;
      }
      if (idx == schema.size() - 1) {
        TypeError::New(info.Env(), "Missing property: " + p)
          .ThrowAsJavaScriptException();
        return;
      }
    }
    switch (schema[idx].second) {
      case TypeKind::BYTE:
      case TypeKind::INT:
      case TypeKind::SHORT:
      case TypeKind::LONG: {
        AddNumberType(
          info.Env(), row->fields[idx], batchOffset, value.Get(p));
        break;
      }
      case TypeKind::VARCHAR:
      case TypeKind::CHAR:
      case TypeKind::STRING:
      case TypeKind::BINARY: {
        AddStringType(info.Env(), row->fields[idx], buffer.get(), batchOffset, &bufferOffset, value.Get(p));
        break;
      }

      case TypeKind::BOOLEAN: {
        AddBoolType(info.Env(), row->fields[idx], batchOffset, value.Get(p));
        break;
      }
      case TypeKind::FLOAT:
      case TypeKind::DOUBLE: {
        AddFloatType(info.Env(), row->fields[idx], batchOffset, value.Get(p));
        break;
      }
      case TypeKind::TIMESTAMP: {
        AddTimeType(info.Env(), row->fields[idx], batchOffset, value.Get(p));
        break;
      }
      case TypeKind::DECIMAL: {
        AddDecimalType(info.Env(),row->fields[idx], type->getSubtype(static_cast<uint64_t>(idx)), batchOffset, idx, value.Get(p));
        break;
      }
      case TypeKind::DATE: {
        AddDateType(info.Env(), row->fields[idx], batchOffset, value.Get(p));
        break;
      }
      case TypeKind::LIST:
      case TypeKind::MAP:
      case TypeKind::STRUCT:
      case TypeKind::UNION: {
        Error::New(
          info.Env(),
          "List, Map, Struct, and Union types are not currently supported")
          .ThrowAsJavaScriptException();
        break;
      }
    }
  }

  batchOffset++;
}

void
Writer::Close(const CallbackInfo&)
{
  if (batchOffset > 0) {
    auto row = dynamic_cast<StructVectorBatch*>(batch.get());
    row->numElements = batchOffset;
    writer->add(*batch);
  }
  writer->close();
}
class ImportCSVWorker : public AsyncWorker
{
public:
  ImportCSVWorker(Function& cb, norc::Writer& self, string csv)
    : AsyncWorker(cb)
    , writer(self)
    , csv(std::move(csv))
  {}

private:
  Writer& writer;
  string csv;

  string columnString(const string& v, uint64_t idx)
  {
    uint64_t col = 0;
    size_t start = 0;
    size_t end = v.find(',');
    while (col < idx && end != string::npos) {
      start = end + 1;
      end = v.find(',', start);
      ++col;
    }
    return col == idx ? v.substr(start, end - start) : "";
  }
  void setLongTypeValue(const vector<string>& data,
                        ColumnVectorBatch* batch,
                        uint64_t valuesRead,
                        uint64_t idx)
  {
    auto longBatch = dynamic_cast<LongVectorBatch*>(batch);
    auto hasNull = false;
    for (uint64_t i = 0; i < valuesRead; i++) {
      string csvCol = columnString(data[i], idx);
      if (csvCol.empty()) {
        batch->notNull[i] = 0;
        hasNull = true;
      } else {
        batch->notNull[i] = 1;
        char* out;
        longBatch->data[i] = strtoll(csvCol.c_str(), nullptr, 10);
      }
    }
    longBatch->hasNulls = hasNull;
    longBatch->numElements = valuesRead;
  }

  void setStringTypeValue(const vector<string>& data,
                          ColumnVectorBatch* batch,
                          uint64_t valuesRead,
                          uint64_t idx,
                          DataBuffer<char>& buffer,
                          uint64_t& offset)
  {
    auto stringBatch = dynamic_cast<StringVectorBatch*>(batch);
    bool hasNull = false;
    for (uint64_t i = 0; i < valuesRead; i++) {
      auto csvCol = columnString(data[i], idx);
      if (csvCol.empty()) {
        batch->notNull[i] = 0;
        hasNull = true;
      } else {
        batch->notNull[i] = 1;
        if (buffer.size() - offset < csvCol.size()) {
          buffer.reserve(buffer.size() * 2);
        }
        memcpy(buffer.data() + offset, csvCol.c_str(), csvCol.size());
        stringBatch->data[i] = buffer.data() + offset;
        stringBatch->length[i] = static_cast<uint64_t>(csvCol.size());
        offset += csvCol.size();
      }
    }
    stringBatch->hasNulls = hasNull;
    stringBatch->numElements = valuesRead;
  }
  void setDoubleTypeValues(const vector<string>& data,
                           ColumnVectorBatch* batch,
                           uint64_t numValues,
                           uint64_t colIndex)
  {
    auto dblBatch = dynamic_cast<DoubleVectorBatch*>(batch);
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      std::string col = columnString(data[i], colIndex);
      if (col.empty()) {
        batch->notNull[i] = 0;
        hasNull = true;
      } else {
        batch->notNull[i] = 1;
        dblBatch->data[i] = strtod(col.c_str(), nullptr); // atof(col.c_str());
      }
    }
    dblBatch->hasNulls = hasNull;
    dblBatch->numElements = numValues;
  }

  // parse fixed point decimal numbers
  void setDecimalTypeValue(const vector<string>& data,
                           ColumnVectorBatch* batch,
                           uint64_t numValues,
                           uint64_t colIndex,
                           size_t scale,
                           size_t precision)
  {

    Decimal128VectorBatch* d128Batch = nullptr;
    Decimal64VectorBatch* d64Batch = nullptr;
    if (precision <= 18) {
      d64Batch = dynamic_cast<Decimal64VectorBatch*>(batch);
      d64Batch->scale = static_cast<int32_t>(scale);
    } else {
      d128Batch = dynamic_cast<Decimal128VectorBatch*>(batch);
      d128Batch->scale = static_cast<int32_t>(scale);
    }
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      string col = columnString(data[i], colIndex);
      if (col.empty()) {
        batch->notNull[i] = 0;
        hasNull = true;
      } else {
        batch->notNull[i] = 1;
        size_t ptPos = col.find('.');
        size_t curScale = 0;
        string num = col;
        if (ptPos != string::npos) {
          curScale = col.length() - ptPos - 1;
          num = col.substr(0, ptPos) + col.substr(ptPos + 1);
        }
        Int128 decimal(num);
        while (curScale != scale) {
          curScale++;
          decimal *= 10;
        }
        if (precision <= 18) {
          d64Batch->values[i] = decimal.toLong();
        } else {
          d128Batch->values[i] = decimal;
        }
      }
    }
    batch->hasNulls = hasNull;
    batch->numElements = numValues;
  }

  void setBoolTypeValue(const vector<string>& data,
                        ColumnVectorBatch* batch,
                        uint64_t numValues,
                        uint64_t colIndex)
  {
    auto boolBatch = dynamic_cast<LongVectorBatch*>(batch);
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      string col = columnString(data[i], colIndex);
      if (col.empty()) {
        batch->notNull[i] = 0;
        hasNull = true;
      } else {
        batch->notNull[i] = 1;
        std::transform(col.begin(), col.end(), col.begin(), ::tolower);
        boolBatch->data[i] = col == "true" || col == "t";
      }
    }
    boolBatch->hasNulls = hasNull;
    boolBatch->numElements = numValues;
  }

  // parse date string from format YYYY-mm-dd
  void setDateTypeValue(const std::vector<std::string>& data,
                        orc::ColumnVectorBatch* batch,
                        uint64_t numValues,
                        uint64_t colIndex)
  {
    auto* longBatch = dynamic_cast<orc::LongVectorBatch*>(batch);
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      std::string col = columnString(data[i], colIndex);
      if (col.empty()) {
        batch->notNull[i] = 0;
        hasNull = true;
      } else {
        batch->notNull[i] = 1;
        struct tm tm
        {};
        memset(&tm, 0, sizeof(struct tm));
        strptime(col.c_str(), "%Y-%m-%d", &tm);
        time_t t = mktime(&tm);
        time_t t1970 = 0;
        double seconds = difftime(t, t1970);
        auto days = static_cast<int64_t>(seconds / (60 * 60 * 24));
        longBatch->data[i] = days;
      }
    }
    longBatch->hasNulls = hasNull;
    longBatch->numElements = numValues;
  }
  void setTimestampTypeValue(const std::vector<std::string>& data,
                             orc::ColumnVectorBatch* batch,
                             uint64_t numValues,
                             uint64_t colIndex)
  {
    struct tm timeStruct
    {};
    auto* tsBatch = dynamic_cast<orc::TimestampVectorBatch*>(batch);
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      std::string col = columnString(data[i], colIndex);
      if (col.empty()) {
        batch->notNull[i] = 0;
        hasNull = true;
      } else {
        memset(&timeStruct, 0, sizeof(timeStruct));
        char* left = strptime(col.c_str(), "%Y-%m-%d %H:%M:%S", &timeStruct);
        if (left == nullptr) {
          batch->notNull[i] = 0;
        } else {
          batch->notNull[i] = 1;
          tsBatch->data[i] = timegm(&timeStruct);
          char* tail;
          double d = strtod(left, &tail);
          if (tail != left) {
            tsBatch->nanoseconds[i] = static_cast<long>(d * 1000000000.0);
          } else {
            tsBatch->nanoseconds[i] = 0;
          }
        }
      }
    }
    tsBatch->hasNulls = hasNull;
    tsBatch->numElements = numValues;
  }

protected:
  void Execute() override
  {
    ifstream source(csv.c_str());
    if (!source.good()) {
      SetError("Unable to open/read csv file");
      return;
    }
    bool eof = false;
    string line;
    vector<string> data;
    unique_ptr<ColumnVectorBatch> row = writer.writer->createRowBatch(1024);
    DataBuffer<char> buffer(*getDefaultPool(), 4 * 1024 * 1024);
    while (!eof) {
      uint64_t valuesRead = 0;
      uint64_t bufferOffset = 0;
      data.clear();
      memset(row->notNull.data(), 1, 1024);
      for (uint64_t i = 0; i < 1024; i++) {
        if (!std::getline(source, line)) {
          eof = true;
          break;
        }
        data.emplace_back(line);
        ++valuesRead;
      }
      if (valuesRead > 0) {
        auto batch = dynamic_cast<StructVectorBatch*>(row.get());
        batch->numElements = valuesRead;
        for (uint64_t i = 0; i < batch->fields.size(); i++) {
          auto subType = writer.type->getSubtype(i);
          switch (subType->getKind()) {
            case BYTE:
            case TypeKind::INT:
            case SHORT:
            case LONG:
              setLongTypeValue(data, batch->fields[i], valuesRead, i);
              break;
            case TypeKind::STRING:
            case TypeKind::VARCHAR:
            case TypeKind::CHAR:
            case TypeKind::BINARY:
              setStringTypeValue(
                data, batch->fields[i], valuesRead, i, buffer, bufferOffset);
              break;

            case TypeKind::FLOAT:
            case TypeKind::DOUBLE:
              setDoubleTypeValues(data, batch->fields[i], valuesRead, i);
              break;

            case TypeKind::BOOLEAN:
              setBoolTypeValue(data, batch->fields[i], valuesRead, i);
              break;

            case TypeKind::DECIMAL:
              setDecimalTypeValue(data,
                                  batch->fields[i],
                                  valuesRead,
                                  i,
                                  subType->getScale(),
                                  subType->getPrecision());
              break;

            case TypeKind::TIMESTAMP:
              setTimestampTypeValue(data, batch->fields[i], valuesRead, i);
              break;
            case TypeKind::DATE:
              setDateTypeValue(data, batch->fields[i], valuesRead, i);
              break;

            case LIST:
            case TypeKind::MAP:
            case TypeKind::STRUCT:
            case TypeKind::UNION:
              SetError(subType->toString() + " is not yet supported");
              break;
          }
        }
        writer.writer->add(*row);
      }
    }
  }
  void OnOK() override
  {
    HandleScope scope(Env());
    Callback().Call({ Env().Undefined(), writer.Value() });
  }
};
void
Writer::ImportCSV(const CallbackInfo& info)
{
  if (!writer) {
    Error::New(info.Env(), "A schema must be defined before importing csv data")
      .ThrowAsJavaScriptException();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
    Error::New(info.Env(), "File path and callback are required")
      .ThrowAsJavaScriptException();
  }
  auto cb = info[1].As<Function>();
  auto worker = new ImportCSVWorker(cb, *this, info[0].As<String>());
  worker->Queue();
}
Napi::Value
Writer::Data(const CallbackInfo& info)
{
  auto file = dynamic_cast<MemoryWriter*>(output.get());
  return Buffer<char>::New(info.Env(), file->getData(), file->getLength());
}
void
Writer::Merge(const CallbackInfo& info)
{
  string filepath;
  Buffer<char> buffer;
  Function condition;
  ReaderOptions options;
  RowReaderOptions rowReaderOptions;
  unique_ptr<Reader> reader;
  unique_ptr<RowReader> rowReader;
  unique_ptr<ColumnVectorBatch> batch;
  bool hasCondition = false;

  if (info[0].IsString()) {
    filepath = info[0].As<String>();
    fs::path p(filepath);
    if (!fs::exists(p)) {
      Error::New(info.Env(), "File not found").ThrowAsJavaScriptException();
      return;
    }
    reader = createReader(readFile(filepath), options);
  } else if(info[0].IsBuffer()) {
    buffer = info[0].As<Buffer<char>>();
    unique_ptr<InputStream> input(
      new MemoryReader(buffer.Data(), buffer.Length()));
    options.setMemoryPool(*getDefaultPool());
    reader = createReader(std::move(input), options);
  } else {
    Error::New(info.Env(), "A string or Buffer was expected").ThrowAsJavaScriptException();
    return;
  }
  if (info.Length() >= 2 && info[1].IsFunction()) {
    condition = info[1].As<Function>();
    hasCondition = true;
  }
  rowReader = reader->createRowReader(rowReaderOptions);
  batch = rowReader->createRowBatch(1024);
  string line;
  unique_ptr<ColumnPrinter> printer =
    createColumnPrinter(line, &rowReader->getSelectedType());
  while (rowReader->next(*batch)) {
    printer->reset(*batch);
    for (unsigned int i = 0; i < batch->numElements; i++) {
      printer->printRow(i);
      auto target = Napi::Object::New(info.Env());
      LineToJSON(info.Env(), line, target);
      bool skip = false;
      if (hasCondition) {
        Napi::Value keep = condition.Call({ target });
        skip = !keep.As<Boolean>();
      }
      if(!skip) {
        AddObject(info, target);
      }
      line = "";
    }
  }
}

}
