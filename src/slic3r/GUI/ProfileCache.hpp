#pragma once

#include <type_traits>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/PresetBundle.hpp"

namespace Slic3r { namespace GUI { namespace ProfileCache {

namespace {

inline std::string delimiter("::");

// Remember all of the (vendor_name::profile_name, profile_path) pairs that have been parsed.
// If a later profile inherits from one of these, the path can be looked up to
// get inherited (key, value) pairs.
class VendorProfilePathCache {
public:
    static void store(std::string vendor_name, std::string profile_name, boost::filesystem::path profile_path) {
        std::string vpn = vendor_name + delimiter + profile_name;
        if (_vendorProfilePathMap.find(vpn) != _vendorProfilePathMap.end()) {
            if (_vendorProfilePathMap[vpn] != profile_path) {
                throw std::runtime_error(std::string("Found a duplicate profile name:\n")
                                         + "    profile=" + vpn + "\n"
                                         + "    previous path=" + _vendorProfilePathMap[vpn].string() + "\n"
                                         + "    current path=" + profile_path.string());
            }
        } else {
            _vendorProfilePathMap[vpn] = profile_path;
        }
    }

    static bool contains(std::string vendor_name, std::string profile_name) {
        std::string vpn = vendor_name + delimiter + profile_name;
        return _vendorProfilePathMap.find(vpn) != _vendorProfilePathMap.end();
    }

    static boost::filesystem::path recall(std::string vendor_name, std::string profile_name) {
        std::string vpn = vendor_name + delimiter + profile_name;
        if (_vendorProfilePathMap.find(vpn) == _vendorProfilePathMap.end()) {
            throw std::runtime_error(std::string("profile ") + vpn + " has not been parsed yet");
        }
        return _vendorProfilePathMap[vpn];
    }

protected:
    inline static std::map<std::string, boost::filesystem::path> _vendorProfilePathMap;
};

// Remember all of the (vendor_name, vendor_path) pairs that have been parsed.
// This data is used if a profile under one vendor inherits from a profile under a previous vendor.
class VendorPathCache {
public:
    static void store(std::string vendor_name, boost::filesystem::path vendor_path) {
        if (_vendorPathMap.find(vendor_name) != _vendorPathMap.end()) {
            if (_vendorPathMap[vendor_name] != vendor_path) {
                throw std::runtime_error(std::string("Found a duplicate vendor name:\n")
                                         + "    vendor name=" + vendor_name + "\n"
                                         + "    previous path=" + _vendorPathMap[vendor_name].string() + "\n"
                                         + "    current path=" + vendor_path.string());
            }
        } else {
            _vendorPathMap[vendor_name] = vendor_path;
        }
    }

    static bool contains(std::string vendor_name) {
        return _vendorPathMap.find(vendor_name) != _vendorPathMap.end();
    }

    static boost::filesystem::path recall(std::string vendor_name) {
        if (_vendorPathMap.find(vendor_name) == _vendorPathMap.end()) {
            throw std::runtime_error(std::string("vendor_name ") + vendor_name + " has not been parsed yet");
        }
        return _vendorPathMap[vendor_name];
    }

protected:
    inline static std::map<std::string, boost::filesystem::path> _vendorPathMap;
};

// Keep track of all of the (vendor_name, profile_name ) pairs that have been visited
// while searching along a chain of inheritance. This data is used to detect circular
// references in the chain of inheritance.
class CircularInheritanceChecker {
public:
    CircularInheritanceChecker(std::string vendor_name, std::string profile_name) {
        std::string vpn = vendor_name + delimiter + profile_name;
        if (std::find(_stack.begin(), _stack.end(), vpn) != _stack.end()) {
            std::stringstream ss;
            ss << "Detected circular inheritance through the following chain:" << std::endl;
            for (auto name : _stack) {
                ss << "    " << name << std::endl;
            }
            ss << "    " << vpn << std::endl;
            throw std::runtime_error(ss.str());
        }
        _stack.push_back(vpn);
    }

    ~CircularInheritanceChecker() {
        _stack.pop_back();
    }

protected:
    inline static std::vector<std::string> _stack;
};

} // anonymous namespace

// Argument Object Factories for the get() method
template <typename T>
class Copy {
public:
    typedef T value_type;
    Copy(std::string key, T& dest) : _key(key), _dest(dest) { }
    std::string key() const { return _key; }
    void store(json j) { j.get_to(_dest); }
protected:
    std::string _key;
    T& _dest;
};

template <typename T>
class CopyWithDefault : virtual public Copy<T> {
public:
    CopyWithDefault(std::string key, T& dest, const T& default_value) : Copy(key, dest), _default_value(default_value) { }
    void store_default() { _dest = _default_value; }
protected:
    const T& _default_value;
};

template <typename T>
class CopyIndex : virtual public Copy<T> {
public:
    CopyIndex(std::string key, int index, T& dest) : Copy(key, dest), _index(index) { }
    void store(json j) {
        if (!j.is_array()) {
            throw std::runtime_error(std::string("key ") + _key + " is expected to be an array but is actually " + j.type_name());
        }
        Copy::store(j[_index]);
    }
protected:
    int _index;
};

template <typename T>
class CopyIndexWithDefault : public CopyIndex<T>, public CopyWithDefault<T> {
public:
    using CopyIndex::store;
    CopyIndexWithDefault(std::string key, int index, T& dest, const T& default_value)
        : Copy(key, dest), CopyIndex(key, index, dest), CopyWithDefault(key, dest, default_value) { }
};

namespace {
template <typename T>
constexpr bool HasDefault = std::is_base_of<CopyWithDefault<T::value_type>, T>::value;
} // anonymous namespace

// This version is called when there are no more key_dest pairs to fetch.
bool Get(const std::string vendor_name, const json& j) {
    return true;
}

// Given an already-parsed json j, fetch the key into the destination
// reference given by key_dest. Multiple key_dest tuples can be given.
template <typename Head, typename... Tail>
bool Get(const std::string vendor_name, const json& j, Head key_dest, Tail... tail) {
    if (j.contains(key_dest.key())) {
        // The expected key is in the json.
        key_dest.store(j[key_dest.key()]);
    }
    else if (j.contains("inherits")) {
        // The expected key is not in the json, but might come from inheritance.
        json parents = j["inherits"];
        std::vector<std::string> parent_profile_names;
        if (parents.is_array()) {
            parents.get_to(parent_profile_names);
        } else {
            parent_profile_names = {parents};
        }

        // If multiple inheritance, search backwards. Last inherit entry has the highest priority.
        bool found_in_parent = false;
        for (auto it = parent_profile_names.crbegin(); !found_in_parent && it != parent_profile_names.crend(); ++it) {
            std::string parent_profile_name = *it;

            // If the parent_profile_name is of the form parent_vendor::parent_profile,
            // then the inheritance is pointing back to a specific vendor_name.
            // Search only in that vendor library, which must have been parsed already.
            size_t del_pos = parent_profile_name.find(delimiter);
            if (del_pos != std::string::npos) {
                // There is no fallback to OrcaFilamentLibrary if the parent_vendor was explicit.
                std::string parent_vendor_name = parent_profile_name.substr(0, del_pos);
                parent_profile_name = parent_profile_name.substr(del_pos + delimiter.length());

                boost::filesystem::path parent_vendor_path = VendorPathCache::recall(parent_vendor_name);
                boost::filesystem::path parent_profile_path = VendorProfilePathCache::recall(parent_vendor_name, parent_profile_name);

                found_in_parent = Get(parent_vendor_name, parent_vendor_path, parent_profile_name, parent_profile_path, key_dest);
            }
            else {
                // Otherwise the default is to search in the current vendor.
                std::string parent_vendor_name = vendor_name;

                if (!VendorProfilePathCache::contains(parent_vendor_name, parent_profile_name)) {
                    // And if the parent profile is not in the current vendor library,
                    // the final fallback is to check the OrcaFilamentLibrary.
                    parent_vendor_name = std::string(PresetBundle::ORCA_FILAMENT_LIBRARY);
                }

                boost::filesystem::path parent_vendor_path = VendorPathCache::recall(parent_vendor_name);
                boost::filesystem::path parent_profile_path = VendorProfilePathCache::recall(parent_vendor_name, parent_profile_name);

                found_in_parent = Get(parent_vendor_name, parent_vendor_path, parent_profile_name, parent_profile_path, key_dest);
            }
        }

        if (!found_in_parent) {
            // No inheritances provided the key either.
            if constexpr (HasDefault<Head>) {
                // If there is a default, use that.
                key_dest.store_default();
            } else {
                // Otherwise fail.
                return false;
            }
        }
    }
    else if constexpr (HasDefault<Head>) {
        // They key is not in j, and there is no inheritance, but a default value is provided.
        key_dest.store_default();
    }
    else {
        // Json does not contain the expected key, has no inheritances, and no default is provided.
        return false;
    }

    // Get next key_dest pair.
    return Get(vendor_name, j, tail...);
}

// This version is called when there are no more key_dest pairs to fetch.
bool Get(const std::string vendor_name,
         const boost::filesystem::path vendor_path,
         const std::string profile_name,
         const boost::filesystem::path profile_path) {
    return true;
}

// Parse json with profile_name in the file at profile_path.
// Fetch the keys into the destination references given by the tuples in tail.
template <typename... Tail>
bool Get(const std::string vendor_name,
         const boost::filesystem::path vendor_path,
         const std::string profile_name,
         const boost::filesystem::path profile_path,
         Tail... tail) {
    try {
        // Remember this vendor_name -> vendor_path combination.
        VendorPathCache::store(vendor_name, vendor_path);

        // Remember this vendor_name::profile_name -> profile_path combination
        VendorProfilePathCache::store(vendor_name, profile_name, profile_path);

        // Detect if we get back to this same vendor_name::profile_name while following a chain of inheritance.
        CircularInheritanceChecker checker(vendor_name, profile_name);

        // Combine the vendor_path and the profile_path to find the profile in the vendor library.
        const boost::filesystem::path preferred_path = boost::filesystem::absolute(vendor_path / profile_path).make_preferred();
        if (!boost::filesystem::exists(preferred_path)) {
            throw std::runtime_error(std::string("path=") + preferred_path.string() + " does not exist.");
        }

        // Parse the json file one time, then use the other
        // version of this function to get all of the keys.
        boost::nowide::ifstream ifs(preferred_path);
        json j = json::parse(ifs);
        return Get(vendor_name, j, tail...);
    }
    catch (nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse error trying to parse profile:";
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    vendor name=" << vendor_name;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    vendor path=" << vendor_path;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    profile name=" << profile_name;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    profile path=" << profile_path;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    reason=" << err.what();
        return false;
    }
    catch (std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": caught exception trying to parse profile:";
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    vendor name=" << vendor_name;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    vendor path=" << vendor_path;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    profile name=" << profile_name;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    profile path=" << profile_path;
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    reason=" << e.what();
        return false;
    }

    // Something else went wrong and no exception was caught.
    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": error trying to parse profile:";
    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    vendor name=" << vendor_name;
    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    vendor path=" << vendor_path;
    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    profile name=" << profile_name;
    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "    profile path=" << profile_path;
    return false;
}

} } } // namespace Slic3r::GUI::ProfileCache
