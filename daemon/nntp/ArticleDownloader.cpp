/*
 *  This file is part of nzbget. See <http://nzbget.net>.
 *
 *  Copyright (C) 2004 Sven Henkel <sidddy@users.sourceforge.net>
 *  Copyright (C) 2007-2016 Andrey Prygunkov <hugbug@users.sourceforge.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "nzbget.h"
#include "ArticleDownloader.h"
#include "ArticleWriter.h"
#include "Decoder.h"
#include "Log.h"
#include "Options.h"
#include "ServerPool.h"
#include "StatMeter.h"
#include "Util.h"

ArticleDownloader::ArticleDownloader()
{
	debug("Creating ArticleDownloader");

	m_articleWriter.SetOwner(this);
	SetLastUpdateTimeNow();
}

ArticleDownloader::~ArticleDownloader()
{
	debug("Destroying ArticleDownloader");
}

void ArticleDownloader::SetInfoName(const char* infoName)
{
	m_infoName = infoName;
	m_articleWriter.SetInfoName(m_infoName);
}

/*
 * How server management (for one particular article) works:
	- there is a list of failed servers which is initially empty;
	- level is initially 0;

	<loop>
		- request a connection from server pool for current level;
		  Exception: this step is skipped for the very first download attempt, because a
		  level-0 connection is initially passed from queue manager;
		- try to download from server;
		- if connection to server cannot be established or download fails due to interrupted connection,
		  try again (as many times as needed without limit) the same server until connection is OK;
		- if download fails with error "Not-Found" (article or group not found) or with CRC error,
		  add the server to failed server list;
		- if download fails with general failure error (article incomplete, other unknown error
		  codes), try the same server again as many times as defined by option <ArticleRetries>;
		  if all attempts fail, add the server to failed server list;
		- if all servers from current level were tried, increase level;
		- if all servers from all levels were tried, break the loop with failure status.
	<end-loop>
*/
void ArticleDownloader::Run()
{
	debug("Entering ArticleDownloader-loop");

	SetStatus(adRunning);

	m_articleWriter.SetFileInfo(m_fileInfo);
	m_articleWriter.SetArticleInfo(m_articleInfo);
	m_articleWriter.Prepare();

	EStatus status = adFailed;
	int retries = g_Options->GetArticleRetries() > 0 ? g_Options->GetArticleRetries() : 1;
	int remainedRetries = retries;
	ServerPool::RawServerList failedServers;
	failedServers.reserve(g_ServerPool->GetServers()->size());
	NewsServer* wantServer = nullptr;
	NewsServer* lastServer = nullptr;
	int level = 0;
	int serverConfigGeneration = g_ServerPool->GetGeneration();
	bool force = m_fileInfo->GetNzbInfo()->GetForcePriority();

	while (!IsStopped())
	{
		status = adFailed;

		SetStatus(adWaiting);
		while (!m_connection && !(IsStopped() || serverConfigGeneration != g_ServerPool->GetGeneration()))
		{
			m_connection = g_ServerPool->GetConnection(level, wantServer, &failedServers);
			usleep(5 * 1000);
		}
		SetLastUpdateTimeNow();
		SetStatus(adRunning);

		if (IsStopped() || ((g_Options->GetPauseDownload() || g_Options->GetQuotaReached()) && !force) ||
			(g_Options->GetTempPauseDownload() && !m_fileInfo->GetExtraPriority()) ||
			serverConfigGeneration != g_ServerPool->GetGeneration())
		{
			status = adRetry;
			break;
		}

		lastServer = m_connection->GetNewsServer();
		level = lastServer->GetNormLevel();

		m_connection->SetSuppressErrors(false);

		m_connectionName.Format("%s (%s)",
			m_connection->GetNewsServer()->GetName(), m_connection->GetHost());

		// check server retention
		bool retentionFailure = m_connection->GetNewsServer()->GetRetention() > 0 &&
			(Util::CurrentTime() - m_fileInfo->GetTime()) / 86400 > m_connection->GetNewsServer()->GetRetention();
		if (retentionFailure)
		{
			detail("Article %s @ %s failed: out of server retention (file age: %i, configured retention: %i)",
				*m_infoName, *m_connectionName,
				(int)(Util::CurrentTime() - m_fileInfo->GetTime()) / 86400,
				m_connection->GetNewsServer()->GetRetention());
			status = adFailed;
			FreeConnection(true);
		}

		if (m_connection && !IsStopped())
		{
			detail("Downloading %s @ %s", *m_infoName, *m_connectionName);
		}

		// test connection
		bool connected = m_connection && m_connection->Connect();
		if (connected && !IsStopped())
		{
			NewsServer* newsServer = m_connection->GetNewsServer();

			// Download article
			status = Download();

			if (status == adFinished || status == adFailed || status == adNotFound || status == adCrcError)
			{
				m_serverStats.StatOp(newsServer->GetId(), status == adFinished ? 1 : 0, status == adFinished ? 0 : 1, ServerStatList::soSet);
			}
		}

		if (m_connection)
		{
			AddServerData();
		}

		if (!connected && m_connection)
		{
			detail("Article %s @ %s failed: could not establish connection", *m_infoName, *m_connectionName);
		}

		if (status == adConnectError)
		{
			connected = false;
			status = adFailed;
		}

		if (connected && status == adFailed)
		{
			remainedRetries--;
		}

		bool optionalBlocked = false;
		if (!connected && m_connection && !IsStopped())
		{
			g_ServerPool->BlockServer(lastServer);
			optionalBlocked = lastServer->GetOptional();
		}

		wantServer = nullptr;
		if (connected && status == adFailed && remainedRetries > 0 && !retentionFailure)
		{
			wantServer = lastServer;
		}
		else
		{
			FreeConnection(status == adFinished || status == adNotFound);
		}

		if (status == adFinished || status == adFatalError)
		{
			break;
		}

		if (IsStopped() || ((g_Options->GetPauseDownload() || g_Options->GetQuotaReached()) && !force) ||
			(g_Options->GetTempPauseDownload() && !m_fileInfo->GetExtraPriority()) ||
			serverConfigGeneration != g_ServerPool->GetGeneration())
		{
			status = adRetry;
			break;
		}

		if (!wantServer && (connected || retentionFailure || optionalBlocked))
		{
			if (!optionalBlocked)
			{
				failedServers.push_back(lastServer);
			}

			// if all servers from current level were tried, increase level
			// if all servers from all levels were tried, break the loop with failure status

			bool allServersOnLevelFailed = true;
			for (NewsServer* candidateServer : g_ServerPool->GetServers())
			{
				if (candidateServer->GetNormLevel() == level)
				{
					bool serverFailed = !candidateServer->GetActive() || candidateServer->GetMaxConnections() == 0 ||
						(candidateServer->GetOptional() && g_ServerPool->IsServerBlocked(candidateServer));
					if (!serverFailed)
					{
						for (NewsServer* ignoreServer : failedServers)
						{
							if (ignoreServer == candidateServer ||
								(ignoreServer->GetGroup() > 0 && ignoreServer->GetGroup() == candidateServer->GetGroup() &&
								 ignoreServer->GetNormLevel() == candidateServer->GetNormLevel()))
							{
								serverFailed = true;
								break;
							}
						}
					}
					if (!serverFailed)
					{
						allServersOnLevelFailed = false;
						break;
					}
				}
			}

			if (allServersOnLevelFailed)
			{
				if (level < g_ServerPool->GetMaxNormLevel())
				{
					detail("Article %s @ all level %i servers failed, increasing level", *m_infoName, level);
					level++;
				}
				else
				{
					detail("Article %s @ all servers failed", *m_infoName);
					status = adFailed;
					break;
				}
			}

			remainedRetries = retries;
		}
	}

	FreeConnection(status == adFinished);

	if (m_articleWriter.GetDuplicate())
	{
		status = adFinished;
	}

	if (status != adFinished && status != adRetry)
	{
		status = adFailed;
	}

	if (IsStopped())
	{
		detail("Download %s cancelled", *m_infoName);
		status = adRetry;
	}

	if (status == adFailed)
	{
		detail("Download %s failed", *m_infoName);
	}

	SetStatus(status);
	Notify(nullptr);

	debug("Exiting ArticleDownloader-loop");
}

ArticleDownloader::EStatus ArticleDownloader::Download()
{
	const char* response = nullptr;
	EStatus status = adRunning;
	m_writingStarted = false;
	m_articleInfo->SetCrc(0);

	if (m_connection->GetNewsServer()->GetJoinGroup())
	{
		// change group
		for (CString& group : m_fileInfo->GetGroups())
		{
			response = m_connection->JoinGroup(group);
			if (response && !strncmp(response, "2", 1))
			{
				break;
			}
		}

		status = CheckResponse(response, "could not join group");
		if (status != adFinished)
		{
			return status;
		}
	}

	// retrieve article
	for (int retry = 3; retry > 0; retry--)
	{
		response = m_connection->Request(BString<1024>("ARTICLE %s\r\n", m_articleInfo->GetMessageId()));
		if ((response && !strncmp(response, "2", 1)) || m_connection->GetAuthError())
		{
			break;
		}
	}

	status = CheckResponse(response, "could not fetch article");
	if (status != adFinished)
	{
		return status;
	}

	if (g_Options->GetDecode())
	{
		m_yDecoder.Clear();
		m_yDecoder.SetCrcCheck(g_Options->GetCrcCheck());
		m_uDecoder.Clear();
	}

	bool body = false;
	bool end = false;
	CharBuffer lineBuf(1024*10);
	status = adRunning;

	while (!IsStopped())
	{
		time_t oldTime = m_lastUpdateTime;
		SetLastUpdateTimeNow();
		if (oldTime != m_lastUpdateTime)
		{
			AddServerData();
		}

		// Throttle the bandwidth
		while (!IsStopped() && (g_Options->GetDownloadRate() > 0.0f) &&
			(g_StatMeter->CalcCurrentDownloadSpeed() > g_Options->GetDownloadRate() ||
			g_StatMeter->CalcMomentaryDownloadSpeed() > g_Options->GetDownloadRate()))
		{
			SetLastUpdateTimeNow();
			usleep(10 * 1000);
		}

		int len = 0;
		char* line = m_connection->ReadLine(lineBuf, lineBuf.Size(), &len);

		g_StatMeter->AddSpeedReading(len);
		if (g_Options->GetAccurateRate())
		{
			AddServerData();
		}

		// Have we encountered a timeout?
		if (!line)
		{
			if (!IsStopped())
			{
				detail("Article %s @ %s failed: Unexpected end of article", *m_infoName, *m_connectionName);
			}
			status = adFailed;
			break;
		}

		//detect end of article
		if (!strcmp(line, ".\r\n") || !strcmp(line, ".\n"))
		{
			end = true;
			break;
		}

		//detect lines starting with "." (marked as "..")
		if (!strncmp(line, "..", 2))
		{
			line++;
			len--;
		}

		if (!body)
		{
			// detect body of article
			if (*line == '\r' || *line == '\n')
			{
				body = true;
			}
			// check id of returned article
			else if (!strncmp(line, "Message-ID: ", 12))
			{
				char* p = line + 12;
				if (strncmp(p, m_articleInfo->GetMessageId(), strlen(m_articleInfo->GetMessageId())))
				{
					if (char* e = strrchr(p, '\r')) *e = '\0'; // remove trailing CR-character
					detail("Article %s @ %s failed: Wrong message-id, expected %s, returned %s", *m_infoName,
						*m_connectionName, m_articleInfo->GetMessageId(), p);
					status = adFailed;
					break;
				}
			}
		}

		if (m_format == Decoder::efUnknown && g_Options->GetDecode())
		{
			m_format = Decoder::DetectFormat(line, len, body);
			if (m_format != Decoder::efUnknown)
			{
				// sometimes news servers misbehave and send article body without new line separator between headers and body
				// if we found decoder signature we know the body is already arrived
				body = true;
			}
		}

		// write to output file
		if (((body && m_format != Decoder::efUnknown) || !g_Options->GetDecode()) && !Write(line, len))
		{
			status = adFatalError;
			break;
		}
	}

	if (!end && status == adRunning && !IsStopped())
	{
		detail("Article %s @ %s failed: article incomplete", *m_infoName, *m_connectionName);
		status = adFailed;
	}

	if (IsStopped())
	{
		status = adFailed;
	}

	if (status == adRunning)
	{
		FreeConnection(true);
		status = DecodeCheck();
	}

	if (m_writingStarted)
	{
		m_articleWriter.Finish(status == adFinished);
	}

	if (status == adFinished)
	{
		detail("Successfully downloaded %s", *m_infoName);
	}

	return status;
}

ArticleDownloader::EStatus ArticleDownloader::CheckResponse(const char* response, const char* comment)
{
	if (!response)
	{
		if (!IsStopped())
		{
			detail("Article %s @ %s failed, %s: Connection closed by remote host",
				*m_infoName, *m_connectionName, comment);
		}
		return adConnectError;
	}
	else if (m_connection->GetAuthError() || !strncmp(response, "400", 3) || !strncmp(response, "499", 3))
	{
		detail("Article %s @ %s failed, %s: %s", *m_infoName, *m_connectionName, comment, response);
		return adConnectError;
	}
	else if (!strncmp(response, "41", 2) || !strncmp(response, "42", 2) || !strncmp(response, "43", 2))
	{
		detail("Article %s @ %s failed, %s: %s", *m_infoName, *m_connectionName, comment, response);
		return adNotFound;
	}
	else if (!strncmp(response, "2", 1))
	{
		// OK
		return adFinished;
	}
	else
	{
		// unknown error, no special handling
		detail("Article %s @ %s failed, %s: %s", *m_infoName, *m_connectionName, comment, response);
		return adFailed;
	}
}

bool ArticleDownloader::Write(char* line, int len)
{
	const char* articleFilename = nullptr;
	int64 articleFileSize = 0;
	int64 articleOffset = 0;
	int articleSize = 0;

	if (g_Options->GetDecode())
	{
		if (m_format == Decoder::efYenc)
		{
			len = m_yDecoder.DecodeBuffer(line, len);
			articleFilename = m_yDecoder.GetArticleFilename();
			articleFileSize = m_yDecoder.GetSize();
		}
		else if (m_format == Decoder::efUx)
		{
			len = m_uDecoder.DecodeBuffer(line, len);
			articleFilename = m_uDecoder.GetArticleFilename();
		}
		else
		{
			detail("Decoding %s failed: unsupported encoding", *m_infoName);
			return false;
		}

		if (len > 0 && m_format == Decoder::efYenc)
		{
			if (m_yDecoder.GetBegin() == 0 || m_yDecoder.GetEnd() == 0)
			{
				return false;
			}
			articleOffset = m_yDecoder.GetBegin() - 1;
			articleSize = (int)(m_yDecoder.GetEnd() - m_yDecoder.GetBegin() + 1);
		}
	}

	if (!m_writingStarted && len > 0)
	{
		if (!m_articleWriter.Start(m_format, articleFilename, articleFileSize, articleOffset, articleSize))
		{
			return false;
		}
		m_writingStarted = true;
	}

	bool ok = len == 0 || m_articleWriter.Write(line, len);

	return ok;
}

ArticleDownloader::EStatus ArticleDownloader::DecodeCheck()
{
	if (g_Options->GetDecode())
	{
		Decoder* decoder = nullptr;
		if (m_format == Decoder::efYenc)
		{
			decoder = &m_yDecoder;
		}
		else if (m_format == Decoder::efUx)
		{
			decoder = &m_uDecoder;
		}
		else
		{
			detail("Decoding %s failed: no binary data or unsupported encoding format", *m_infoName);
			return adFailed;
		}

		Decoder::EStatus status = decoder->Check();

		if (status == Decoder::dsFinished)
		{
			if (decoder->GetArticleFilename())
			{
				m_articleFilename = decoder->GetArticleFilename();
			}

			if (m_format == Decoder::efYenc)
			{
				m_articleInfo->SetCrc(g_Options->GetCrcCheck() ?
					m_yDecoder.GetCalculatedCrc() : m_yDecoder.GetExpectedCrc());
			}

			return adFinished;
		}
		else if (status == Decoder::dsCrcError)
		{
			detail("Decoding %s failed: CRC-Error", *m_infoName);
			return adCrcError;
		}
		else if (status == Decoder::dsArticleIncomplete)
		{
			detail("Decoding %s failed: article incomplete", *m_infoName);
			return adFailed;
		}
		else if (status == Decoder::dsInvalidSize)
		{
			detail("Decoding %s failed: size mismatch", *m_infoName);
			return adFailed;
		}
		else if (status == Decoder::dsNoBinaryData)
		{
			detail("Decoding %s failed: no binary data found", *m_infoName);
			return adFailed;
		}
		else
		{
			detail("Decoding %s failed", *m_infoName);
			return adFailed;
		}
	}
	else
	{
		return adFinished;
	}
}

void ArticleDownloader::SetLastUpdateTimeNow()
{
	m_lastUpdateTime = Util::CurrentTime();
}

void ArticleDownloader::LogDebugInfo()
{
	info("      Download: status=%i, LastUpdateTime=%s, InfoName=%s", m_status,
		 *Util::FormatTime(m_lastUpdateTime), *m_infoName);
}

void ArticleDownloader::Stop()
{
	debug("Trying to stop ArticleDownloader");
	Thread::Stop();
	Guard guard(m_connectionMutex);
	if (m_connection)
	{
		m_connection->SetSuppressErrors(true);
		m_connection->Cancel();
	}
	debug("ArticleDownloader stopped successfully");
}

bool ArticleDownloader::Terminate()
{
	NntpConnection* connection = m_connection;
	bool terminated = Kill();
	if (terminated && connection)
	{
		debug("Terminating connection");
		connection->SetSuppressErrors(true);
		connection->Cancel();
		connection->Disconnect();
		g_StatMeter->AddServerData(connection->FetchTotalBytesRead(), connection->GetNewsServer()->GetId());
		g_ServerPool->FreeConnection(connection, true);
	}
	return terminated;
}

void ArticleDownloader::FreeConnection(bool keepConnected)
{
	if (m_connection)
	{
		debug("Releasing connection");
		Guard guard(m_connectionMutex);
		if (!keepConnected || m_connection->GetStatus() == Connection::csCancelled)
		{
			m_connection->Disconnect();
		}
		AddServerData();
		g_ServerPool->FreeConnection(m_connection, true);
		m_connection = nullptr;
	}
}

void ArticleDownloader::AddServerData()
{
	int bytesRead = m_connection->FetchTotalBytesRead();
	g_StatMeter->AddServerData(bytesRead, m_connection->GetNewsServer()->GetId());
	m_downloadedSize += bytesRead;
}
