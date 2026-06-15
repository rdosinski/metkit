/*
 * (C) Copyright 1996- ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 * In applying this licence, ECMWF does not waive the privileges and immunities
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */

#include "metkit/odb/OdbMetadataDecoder.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "eckit/config/Resource.h"
#include "eckit/config/LocalConfiguration.h"
#include "eckit/config/YAMLConfiguration.h"
#include "eckit/utils/StringTools.h"

#include "metkit/config/LibMetkit.h"
#include "metkit/mars/MarsRequest.h"
#include "metkit/mars/Type.h"
#include "metkit/odb/IdMapper.h"


namespace {

struct ConditionalRule {
    std::string keyword;
    std::string value;
};

class OdbColumnNameMapping {

public:

    static OdbColumnNameMapping& instance() {
        static OdbColumnNameMapping mapping;
        return mapping;
    }

    const std::vector<std::string>& columnNames() { return columnNames_; }
    const std::map<std::string, std::string>& table() { return mapping_; }

    bool conditionalRule(const std::string& odbColumnName, ConditionalRule& rule) const {
        auto it = conditional_.find(odbColumnName);
        if (it == conditional_.end()) {
            return false;
        }
        rule = it->second;
        return true;
    }

private:  // methods

    OdbColumnNameMapping() {
        static eckit::PathName configPath(
            eckit::Resource<eckit::PathName>("odbMarsRequestMapping", "~metkit/share/metkit/odb/marsrequest.yaml"));
        eckit::YAMLConfiguration config(configPath);
        for (const auto& key : config.keys()) {
            if (config.isSubConfiguration(key) && eckit::StringTools::endsWith(key, "_conditional")) {
                std::string keyword = eckit::StringTools::lower(
                    key.substr(0, key.size() - std::string("_conditional").size()));
                eckit::LocalConfiguration keywordSub = config.getSubConfiguration(key);
                for (const auto& keywordVal : keywordSub.keys()) {
                    eckit::LocalConfiguration colsSub = keywordSub.getSubConfiguration(keywordVal);
                    for (const auto& marsKey : colsSub.keys()) {
                        std::string odbCol = colsSub.getString(marsKey);
                        mapping_[odbCol] = eckit::StringTools::lower(marsKey);
                        conditional_[odbCol] = ConditionalRule{keyword, eckit::StringTools::lower(keywordVal)};
                    }
                }
            }
            else if (!config.isSubConfiguration(key)) {
                mapping_[config.getString(key)] = eckit::StringTools::lower(key);
            }
        }

        columnNames_.reserve(mapping_.size());
        for (const auto& kv : mapping_) {
            columnNames_.push_back(kv.first);
        }
    }

private:  // members

    std::map<std::string, std::string> mapping_;
    std::map<std::string, ConditionalRule> conditional_;
    std::vector<std::string> columnNames_;
};
}  // namespace

//----------------------------------------------------------------------------------------------------------------------

namespace metkit::codes {

//----------------------------------------------------------------------------------------------------------------------

const std::vector<std::string>& OdbMetadataDecoder::columnNames() {
    return OdbColumnNameMapping::instance().columnNames();
}

std::vector<std::string> OdbMetadataDecoder::columnNames(const odc::api::Frame& frame) {
    std::vector<std::string> cols;
    cols.reserve(OdbColumnNameMapping::instance().columnNames().size());

    for (const auto& c : OdbColumnNameMapping::instance().columnNames()) {
        if (frame.hasColumn(c)) {
            cols.push_back(c);
        }
    }

    return cols;
}

template <typename T>
void OdbMetadataDecoder::visit(const std::string& columnName, const std::set<T>& vals,
                               const metkit::mars::MarsLanguage& language) {

    auto mapitr = OdbColumnNameMapping::instance().table().find(columnName);
    ASSERT(mapitr != OdbColumnNameMapping::instance().table().end());
    std::string keyword   = eckit::StringTools::lower(mapitr->second);
    metkit::mars::Type* t = language.type(keyword);

    ASSERT(options_.valueRepresentation == eckit::message::ValueRepresentation::String);

    for (auto val : vals) {
        std::string stringVal = eckit::Translator<T, std::string>()(val);
        std::string tidyVal   = t->tidy(stringVal);
        if (stringVal == tidyVal)  // if t->tidy had no effect, set the original value
            gather_.setValue(keyword, val);
        else
            gather_.setValue(keyword, tidyVal);
    }
}


OdbMetadataDecoder::OdbMetadataDecoder(eckit::message::MetadataGatherer& gather,
                                       const eckit::message::GetMetadataOptions& options, const std::string& verb) :
    language_(verb), gather_(gather), options_(options) {}

template <typename T>
void OdbMetadataDecoder::visitOrDefer(const std::string& columnName, const std::set<T>& vals) {
    ConditionalRule rule;
    if (OdbColumnNameMapping::instance().conditionalRule(columnName, rule)) {
        deferred_[columnName] = [this, columnName, vals, rule]() {
            auto it = keywordValues_.find(rule.keyword);
            if (it != keywordValues_.end() && it->second.count(rule.value)) {
                visit(columnName, vals, language_);
            }
        };
        return;
    }
    visit(columnName, vals, language_);
}

void OdbMetadataDecoder::operator()(const std::string& columnName, const std::set<long>& vals) {
    LOG_DEBUG_LIB(LibMetkit) << "OdbMetadataDecoder::operator() columnName: " << columnName << " vals: " << vals
                             << std::endl;

    auto mapitr = OdbColumnNameMapping::instance().table().find(columnName);
    ASSERT(mapitr != OdbColumnNameMapping::instance().table().end());
    const std::string keyword = mapitr->second;
    metkit::mars::Type* t     = language_.type(keyword);

    std::set<std::string> mapped;
    if (metkit::odb::IdMapper::instance().alphanumeric(keyword, vals, mapped)) {
        for (const auto& v : mapped) {
            keywordValues_[keyword].insert(eckit::StringTools::lower(t->tidy(v)));
        }
        visitOrDefer(columnName, mapped);
    }
    else {
        for (const auto& v : vals) {
            std::string stringVal = eckit::Translator<long, std::string>()(v);
            keywordValues_[keyword].insert(eckit::StringTools::lower(t->tidy(stringVal)));
        }
        visitOrDefer(columnName, vals);
    }
}

void OdbMetadataDecoder::operator()(const std::string& columnName, const std::set<double>& vals) {
    auto mapitr = OdbColumnNameMapping::instance().table().find(columnName);
    ASSERT(mapitr != OdbColumnNameMapping::instance().table().end());
    const std::string keyword = mapitr->second;
    metkit::mars::Type* t     = language_.type(keyword);
    for (const auto& v : vals) {
        std::string stringVal = eckit::Translator<double, std::string>()(v);
        keywordValues_[keyword].insert(eckit::StringTools::lower(t->tidy(stringVal)));
    }
    visitOrDefer(columnName, vals);
}

void OdbMetadataDecoder::operator()(const std::string& columnName, const std::set<std::string>& vals) {
    auto mapitr = OdbColumnNameMapping::instance().table().find(columnName);
    ASSERT(mapitr != OdbColumnNameMapping::instance().table().end());
    const std::string keyword = mapitr->second;
    metkit::mars::Type* t     = language_.type(keyword);
    for (const auto& v : vals) {
        keywordValues_[keyword].insert(eckit::StringTools::lower(t->tidy(v)));
    }
    visitOrDefer(columnName, vals);
}

void OdbMetadataDecoder::finalize() {
    for (auto& kv : deferred_) {
        kv.second();
    }
    deferred_.clear();
    keywordValues_.clear();
}

//----------------------------------------------------------------------------------------------------------------------

}  // namespace metkit::codes
