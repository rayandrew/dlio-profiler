//
// Created by haridev on 3/28/23.
//

#include <dlio_profiler/core/macro.h>
#include <dlio_profiler/writer/chrome_writer.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cmath>
#include <sstream>
#include <thread>

void dlio_profiler::ChromeWriter::initialize(char *filename, bool throw_error) {
  this->throw_error = throw_error;
  this->filename = filename;
  if (fh == nullptr) {
    fh = fopen(filename, "ab+");
    if (fh == nullptr) {
      ERROR(fh == nullptr, "unable to create log file %s",
            filename);  // GCOVR_EXCL_LINE
    } else {
      setvbuf(fh, NULL, _IOLBF, MAX_LINE_SIZE);
      DLIO_PROFILER_LOGINFO("created log file %s", filename);
    }
  }
  DLIO_PROFILER_LOGDEBUG("ChromeWriter.initialize %s", this->filename.c_str());
}

void dlio_profiler::ChromeWriter::log(
    ConstEventType event_name, ConstEventType category,
    TimeResolution &start_time, TimeResolution &duration,
    std::unordered_map<std::string, std::any> *metadata, ProcessID process_id,
    ThreadID thread_id) {
  DLIO_PROFILER_LOGDEBUG("ChromeWriter.log", "");
  if (fh != nullptr) {
    int size;
    char data[MAX_LINE_SIZE];
    convert_json(event_name, category, start_time, duration, metadata,
                 process_id, thread_id, &size, data);
    write_buffer_op(data, size);
  } else {
    DLIO_PROFILER_LOGERROR("ChromeWriter.log invalid", "");
  }
  is_first_write = false;
}

void dlio_profiler::ChromeWriter::finalize() {
  DLIO_PROFILER_LOGDEBUG("ChromeWriter.finalize", "");
  if (fh != nullptr) {
    DLIO_PROFILER_LOGINFO("Profiler finalizing writer %s", filename.c_str());
    fflush(fh);
    int status = fclose(fh);
    if (status != 0) {
      ERROR(status != 0, "unable to close log file %d for a+",
            filename.c_str());  // GCOVR_EXCL_LINE
    }
    if (index == 0) {
      DLIO_PROFILER_LOGINFO("No trace data written. Deleting file %s",
                            filename.c_str());
      dlp_unlink(filename.c_str());
    } else {
      DLIO_PROFILER_LOGINFO("Profiler writing the final symbol", "");
      fh = fopen(this->filename.c_str(), "r+");
      if (fh == nullptr) {
        ERROR(fh == nullptr, "unable to open log file %s with O_WRONLY",
              this->filename.c_str());  // GCOVR_EXCL_LINE
      } else {
        std::string data = "[\n";
        auto written_elements =
            fwrite(data.c_str(), sizeof(char), data.size(), fh);
        if (written_elements != data.size()) {  // GCOVR_EXCL_START
          ERROR(written_elements != data.size(),
                "unable to finalize log write %s for O_WRONLY written only %d "
                "of %d",
                filename.c_str(), data.size(), written_elements);
        }  // GCOVR_EXCL_STOP
        status = fclose(fh);
        if (status != 0) {
          ERROR(status != 0, "unable to close log file %d for O_WRONLY",
                filename.c_str());  // GCOVR_EXCL_LINE
        }
      }
      if (enable_compression) {
        if (system("which gzip > /dev/null 2>&1")) {
          DLIO_PROFILER_LOGERROR("Gzip compression does not exists",
                                 "");  // GCOVR_EXCL_LINE
        } else {
          DLIO_PROFILER_LOGINFO("Applying Gzip compression on file %s",
                                filename.c_str());
          char cmd[2048];
          sprintf(cmd, "gzip -f %s", filename.c_str());
          int ret = system(cmd);
          if (ret == 0) {
            DLIO_PROFILER_LOGINFO("Successfully compressed file %s.gz",
                                  filename.c_str());
          } else {
            DLIO_PROFILER_LOGERROR("Unable to compress file %s",
                                   filename.c_str());
          }
        }
      }
    }
  }
  if (enable_core_affinity) {
#if DISABLE_HWLOC == 1
    hwloc_topology_destroy(topology);
#endif
  }
  DLIO_PROFILER_LOGDEBUG("Finished writer finalization", "");
}

void dlio_profiler::ChromeWriter::convert_json(
    ConstEventType event_name, ConstEventType category,
    TimeResolution start_time, TimeResolution duration,
    std::unordered_map<std::string, std::any> *metadata, ProcessID process_id,
    ThreadID thread_id, int *size, char *data) {
  std::string is_first_char = "";
  if (is_first_write) is_first_char = "   ";
  if (include_metadata) {
    char metadata_line[MAX_META_LINE_SIZE];
    std::stringstream all_stream;
    auto cores = core_affinity();
    auto cores_size = cores.size();
    if (cores_size > 0) {
      all_stream << ", \"core_affinity\": [";
      for (long unsigned int i = 0; i < cores_size; ++i) {
        all_stream << cores[i];
        if (i < cores_size - 1) all_stream << ",";
      }
      all_stream << "]";
    }
    bool has_meta = false;
    std::stringstream meta_stream;
    auto meta_size = metadata->size();
    long unsigned int i = 0;
    for (auto item : *metadata) {
      has_meta = true;
      if (item.second.type() == typeid(unsigned int)) {
        meta_stream << "\"" << item.first
                    << "\":" << std::any_cast<unsigned int>(item.second);
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(int)) {
        meta_stream << "\"" << item.first
                    << "\":" << std::any_cast<int>(item.second);
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(const char *)) {
        meta_stream << "\"" << item.first << "\":\""
                    << std::any_cast<const char *>(item.second) << "\"";
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(std::string)) {
        meta_stream << "\"" << item.first << "\":\""
                    << std::any_cast<std::string>(item.second) << "\"";
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(size_t)) {
        meta_stream << "\"" << item.first << "\":\""
                    << std::any_cast<size_t>(item.second) << "\"";
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(long)) {
        meta_stream << "\"" << item.first << "\":\""
                    << std::any_cast<long>(item.second) << "\"";
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(ssize_t)) {
        meta_stream << "\"" << item.first << "\":\""
                    << std::any_cast<ssize_t>(item.second) << "\"";
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(off_t)) {
        meta_stream << "\"" << item.first << "\":\""
                    << std::any_cast<off_t>(item.second) << "\"";
        if (i < meta_size - 1) meta_stream << ",";
      } else if (item.second.type() == typeid(off64_t)) {
        meta_stream << "\"" << item.first << "\":\""
                    << std::any_cast<off64_t>(item.second) << "\"";
        if (i < meta_size - 1) meta_stream << ",";
      } else {
        DLIO_PROFILER_LOGINFO("No conversion for type %s", item.first);
      }
      i++;
    }
    if (has_meta) {
      all_stream << "," << meta_stream.str();
    }
    sprintf(metadata_line, R"("hostname":"%s"%s)", this->hostname,
            all_stream.str().c_str());
    *size = snprintf(
        data, MAX_LINE_SIZE,
        "%s{\"id\":\"%d\",\"name\":\"%s\",\"cat\":\"%s\",\"pid\":\"%lu\","
        "\"tid\":\"%lu\",\"ts\":\"%llu\",\"dur\":\"%llu\",\"ph\":\"X\","
        "\"args\":{%s}}\n",
        is_first_char.c_str(), index.load(), event_name, category, process_id,
        thread_id, start_time, duration, metadata_line);
  } else {
    *size = snprintf(
        data, MAX_LINE_SIZE,
        "%s{\"id\":\"%d\",\"name\":\"%s\",\"cat\":\"%s\",\"pid\":\"%lu\","
        "\"tid\":\"%lu\",\"ts\":\"%llu\",\"dur\":\"%llu\",\"ph\":\"X\","
        "\"args\":{}}\n",
        is_first_char.c_str(), index.load(), event_name, category, process_id,
        thread_id, start_time, duration);
  }
  DLIO_PROFILER_LOGDEBUG("ChromeWriter.convert_json %s on %s", data,
                         this->filename.c_str());
  index++;
}
