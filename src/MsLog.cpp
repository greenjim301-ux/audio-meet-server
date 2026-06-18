#include "MsLog.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_LOG_LEN 2048
#define MAX_LOG_QUEUE 512

unique_ptr<MsLog> MsLog::m_log;
mutex MsLog::m_mutex;
condition_variable MsLog::m_condiVar;

MsLog::MsLog() : m_exit(false), m_level(LOG_LEVEL_INFO), m_curDay(-1), m_fp(NULL), m_days(3) {}

MsLog::~MsLog()
{
	if (m_fp)
	{
		fclose(m_fp);
	}

	while (m_logQuePtr.size())
	{
		m_logQuePtr.pop();
	}

	while (m_logBufPtr.size())
	{
		m_logBufPtr.pop();
	}
}

MsLog *MsLog::Instance()
{
	if (MsLog::m_log.get())
	{
		return MsLog::m_log.get();
	}
	else
	{
		lock_guard<mutex> lk(MsLog::m_mutex);

		if (MsLog::m_log.get())
		{
			return MsLog::m_log.get();
		}
		else
		{
			MsLog::m_log = make_unique<MsLog>();
			return MsLog::m_log.get();
		}
	}
}

unique_ptr<char[]> MsLog::GetLogBufPtr()
{
	if (m_logBufPtr.size())
	{
		unique_ptr<char[]> buf = std::move(m_logBufPtr.front());
		m_logBufPtr.pop();
		return buf;
	}
	else
	{
		unique_ptr<char[]> buf = make_unique<char[]>(MAX_LOG_LEN);
		return buf;
	}
}

void MsLog::RelLogBufPtr(unique_ptr<char[]> bufPtr)
{
	if (m_logBufPtr.size() > MAX_LOG_QUEUE)
	{
		// free(buf);
	}
	else
	{
		m_logBufPtr.emplace(std::move(bufPtr));
	}
}

void MsLog::EnqueLogPtr(unique_ptr<char[]> logPtr) { m_logQuePtr.emplace(std::move(logPtr)); }

void MsLog::Log(int level, const char *strLog, ...)
{
	if (level > m_level)
	{
		return;
	}

	unique_lock<mutex> lk(MsLog::m_mutex);

	unique_ptr<char[]> logBuf = this->GetLogBufPtr();
	int maxLen = MAX_LOG_LEN - 1;

	va_list argList;

	va_start(argList, strLog);

	int nRet = vsnprintf(logBuf.get(), maxLen, strLog, argList);

	va_end(argList);

	if (nRet < 0)
	{
		this->RelLogBufPtr(std::move(logBuf));
		lk.unlock();
	}
	else
	{
		if (nRet > maxLen - 1)
		{
			nRet = maxLen - 1;
		}

		logBuf[nRet] = '\n';
		logBuf[nRet + 1] = '\0';

		this->EnqueLogPtr(std::move(logBuf));
		lk.unlock();

		MsLog::m_condiVar.notify_one();
	}
}

void MsLog::OnRun()
{
	// if log directory not exist, create it
	if (access("log", F_OK) == -1)
	{
		mkdir("log", 0755);
	}

	unique_lock<mutex> lk(MsLog::m_mutex);

	while (!m_exit)
	{
		MsLog::m_condiVar.wait(lk);

		while (m_logQuePtr.size() && !m_exit)
		{
			unique_ptr<char[]> log = std::move(m_logQuePtr.front());
			m_logQuePtr.pop();

			lk.unlock();

			time_t tt;
			struct tm *atm;

			time(&tt);
			atm = localtime(&tt);
			bool change = false;

			if (m_curDay != atm->tm_mday)
			{
				change = true;
				bool isAppend = (m_curDay == -1);
				m_curDay = atm->tm_mday;

				if (m_fp)
				{
					fclose(m_fp);
				}

				char fName[32];
				sprintf(fName, "log/log-%d.txt", m_curDay);

				if (isAppend)
				{
					m_fp = fopen(fName, "ab");
				}
				else
				{
					m_fp = fopen(fName, "wb");
				}

				string cmd = "ln -sf log-" + to_string(m_curDay) + ".txt log/log-current.txt";
				system(cmd.c_str());
			}

			fprintf(m_fp, "%04d-%02d-%02d %02d:%02d:%02d %s", atm->tm_year + 1900, atm->tm_mon + 1,
					atm->tm_mday, atm->tm_hour, atm->tm_min, atm->tm_sec, log.get());
			fflush(m_fp);

			if (change)
			{
				tt -= m_days * 86400; // 4 dyas
				atm = localtime(&tt);

				char fName[32];
				sprintf(fName, "log/log-%d.txt", atm->tm_mday);

				unlink(fName);
			}

			lk.lock();

			this->RelLogBufPtr(std::move(log));
		}
	}

	lk.unlock();
}

void MsLog::Exit()
{
	unique_lock<mutex> lk(MsLog::m_mutex);
	m_exit = true;
	lk.unlock();

	m_condiVar.notify_one();
}

void MsLog::SetLevel(int level) { m_level = level; }

void MsLog::Run()
{
	thread worker(&MsLog::OnRun, this);
	worker.detach();
}
