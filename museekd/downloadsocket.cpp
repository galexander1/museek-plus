/*  Museek - A SoulSeek client written in C++
    Copyright (C) 2006-2007 Ingmar K. Steen (iksteen@gmail.com)
    Copyright 2008 little blue poney <lbponey@users.sourceforge.net>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif // HAVE_CONFIG_H
#include "downloadsocket.h"
#include "downloadmanager.h"
#include "museekd.h"
#include "configmanager.h"
#include "ticketsocket.h"
#include <errno.h>
#include <NewNet/nnreactor.h>

static int
file_exists(const char *path)
{
	int fd;
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		close(fd);
		return 1;
	}
	return 0;
}

static const char *
increment_path(const char *path)
{
	static char *buf = NULL;
	static int buf_len = 0;
	int needed = strlen(path) + 20;
	int h_ofs;
	int h_n;
	int i;

	if (needed >= buf_len) {
		free(buf);
		buf_len = needed + 100;
		buf = new char[buf_len];
	}
	strcpy(buf, path);

	h_ofs = -1;
	h_n = 0;
	for (i = buf; buf[i]; i++) {
		if (buf[i] == '-') {
			h_ofs = i;
			h_n = 0;
		} else if (isdigit(buf[i])) {
			if (h_ofs != -1) {
				h_n = (h_n*10) + (buf[i]-'0');
			} else {
				h_n = 0;
			}
		} else {
			h_ofs = -1;
		}
	}
	if (h_ofs > 0) {
		sprintf(&buf[h_ofs+1], "%d", h_n+1);
	} else {
		sprintf(&buf[i], "-1");
	}
	return buf;
}

Museek::DownloadSocket::DownloadSocket(Museek::Museekd * museekd, Museek::Download * download)
              : UserSocket(museekd, "F"), m_Download(download)
{
    // Connect our data received event.
    dataReceivedEvent.connect(this, &DownloadSocket::onDataReceived);
    // Connect disconnected event.
    disconnectedEvent.connect(this, &DownloadSocket::onDisconnected);
    cannotConnectEvent.connect(this, &DownloadSocket::onCannotConnect);
}

Museek::DownloadSocket::~DownloadSocket()
{
    NNLOG("museekd.down.debug", "DownloadSocket destroyed");
    if(m_Output.is_open())
        m_Output.close();
}

/*
    Initiate the download from our side => connect to the peer
*/
void
Museek::DownloadSocket::pickUp()
{
    connectedEvent.connect(this, &DownloadSocket::onConnected);

    // Initiate a connection to the other peer (active or passive).
    initiate(m_Download->user());
    m_Download->setState(TS_Connecting);
}

/*
    Our connection is successful, send ticket and position.
*/
void
Museek::DownloadSocket::onConnected(NewNet::ClientSocket *)
{
    m_Download->setState(TS_Transferring);

    m_DataTimeout = museekd()->reactor()->addTimeout(120000, this, &DownloadSocket::dataTimeout);

    // The send buffer (will hold ticket + offset)
    unsigned char buf[12];

    // Send the ticket.
    for(int i = 0; i < 4; ++i) {
        buf[i] = (m_Download->ticket() >> (i * 8)) & 0xff;
    }

    // Open our incomplete file
    openIncompleteFile();

    // Send the file position.
    uint64 pos = m_Download->position();
    for(int i = 0; i < 8; ++i) {
        buf[i + 4] = (pos >> (i * 8)) & 0xff;
    }
    send(buf, 12);
}

/*
    We got disconnected
*/
void
Museek::DownloadSocket::onDisconnected(ClientSocket *)
{
	NNLOG("museekd.down.debug", "DownloadSocket disconnected");

	if(m_Download->position() >= m_Download->size())
		m_Download->setState(TS_Finished);
	else
		m_Download->setState(TS_ConnectionClosed);

    if(m_Output.is_open())
        m_Output.close();
}

/*
    Called when the connection cannot be established
*/
void
Museek::DownloadSocket::onCannotConnect(ClientSocket * socket)
{
	disconnect();
}

/*
    Wait for the uploader to send us our ticket
*/
void
Museek::DownloadSocket::wait()
{
    // Wait for an incoming connection (via TicketSocket).
    m_Download->setState(TS_Waiting);
    museekd()->downloads()->transferTicketReceivedEvent.connect(this, &DownloadSocket::onTransferTicketReceived);
}

/*
    Stops the downloading
*/
void
Museek::DownloadSocket::stop()
{
    NNLOG("museekd.down.debug", "Disconnecting download socket...");
    disconnect();
}

/*
    We have received the ticket, we can start downloading
*/
void
Museek::DownloadSocket::onTransferTicketReceived(TicketSocket * socket)
{
    if((m_Download->state() == TS_Waiting) && (m_Download->ticket() == socket->ticket()) && (m_Download->user() == socket->user()))
    {
        NNLOG("museekd.down.debug", "*does happy dance* (found a download)");

        // Steal the socket and its data.
        setDescriptor(socket->descriptor());
        setSocketState(SocketConnected);
        receiveBuffer() = socket->receiveBuffer();

        // Open our incomplete file
        openIncompleteFile();

        // Send the file position.
        unsigned char buf[8];
        uint64 pos = m_Download->position();
        for(int i = 0; i < 8; ++i) {
            buf[i] = (pos >> (i * 8)) & 0xff;
        }
        send(buf, 8);

        // Change the state.
        m_Download->setState(TS_Transferring);
    }
}

/*
    Open the incomplete file where the data received will be stored.
*/
bool
Museek::DownloadSocket::openIncompleteFile()
{
    const char *fn = m_Download->incompletePath().c_str();

    if (m_Download->position() == 0) {
	/* it has not started downloading yet, ensure its filename is unique */
	while (file_exists(fn)) {
		fn = increment_path(fn);
	}
	m_Download->setIncompletePath(fn);
    }

    // We received data, open the incomplete file if necessary.
    NNLOG("museekd.down.debug", "Downloading to: %s.", fn);
        m_Output.open(fn, std::ofstream::binary | std::ofstream::app | std::ofstream::ate);
    }
    if(! m_Output.is_open()) {
        // Couldn't open the incomplete file. Bail out.
        NNLOG("museekd.down.warn", "Couldn't open '%s'.", m_Download->incompletePath().c_str());
        stop();
        return false;
    }

    // Set the position of the download to EOF
    m_Download->setPosition(m_Output.tellp());
    NNLOG("museekd.down.debug", "Set position to %llu (%llu).", m_Download->position(), (uint64)m_Output.tellp());

    return true;
}

/*
    Some data has been received
*/
void
Museek::DownloadSocket::onDataReceived(NewNet::ClientSocket * socket)
{
    if (m_Download->state() == TS_Transferring) {
        if (m_DataTimeout.isValid())
            museekd()->reactor()->removeTimeout(m_DataTimeout);

        m_DataTimeout = museekd()->reactor()->addTimeout(60000, this, &DownloadSocket::dataTimeout);

        // Write buffer to disk.
        m_Output.write((const char *)receiveBuffer().data(), receiveBuffer().count());
        // Increase the download counter.
        m_Download->received(receiveBuffer().count());
        // Clear buffer.
        receiveBuffer().clear();

        // Finished?
        if(m_Download->position() >= m_Download->size()) {
            NNLOG("museekd.down.debug", "Download of %s from %s finished.", m_Download->remotePath().c_str(), m_Download->user().c_str());
            // Close output.
            m_Output.close();
            // Rename / move file.
            finish();
            // Disconnect.
            stop();
        }
        else if(m_Output.fail()) {
            stop();
            return;
        }
    }
}

/*
    Call it when the download is complete : move the file from incomplete to complete dir
*/
void
Museek::DownloadSocket::finish()
{
    // Ok, we're done.
    m_Download->setState(TS_Finished);

    const char *destpath = m_Download->destinationPath(true).c_str();

    /* ensure the filename is unique */
    while (file_exists(destpath)) {
	destpath = increment_path(destpath);
    }

#ifdef WIN32
    // On Win32, rename doesn't overwrite an existing file automatically.
    remove(destpath);
#endif // WIN32
    // Rename the incomplete file to the destination path.
    if(rename(m_Download->incompletePath().c_str(), destpath) == -1) {
        if(errno == EXDEV) {
            /* Incomplete and destination path are on different partitions or
             mount points. We'll have to copy it manually. */
            NNLOG("museekd.down.warn", "Having incomplete and download directory on different partitions is a bad idea!");
            // Open the input stream.
            std::ifstream fin;
            fin.open(m_Download->incompletePath().c_str(), std::fstream::in | std::fstream::binary);
            if(! fin.is_open()) {
                NNLOG("museekd.down.warn", "Couldn't open '%s' for reading.", m_Download->incompletePath().c_str());
                return;
            }
            // Open the output stream.
            std::ofstream fout;
            fout.open(destpath, std::fstream::out | std::fstream::binary | std::fstream::trunc);
            if(! fout.is_open()) {
                NNLOG("museekd.down.warn", "Couldn't open '%s' for writing.", destpath);
                fin.close();
                return;
            }

            char buffer[8192];
            std::streamsize n;
            bool ok = true;
            do {
                // Read some bytes.
                n = fin.readsome(buffer, 8192);
                if(fin.fail()) {
                    // Problem...
                    NNLOG("museekd.down.warn", "Couldn't read from '%s'.", m_Download->incompletePath().c_str());
                    ok = false;
                    break;
                }

                if(n) {
                    // Write some bytes.
                    fout.write(buffer, n);
                    if(fout.fail()) {
                        // Problem.
                        NNLOG("museekd.down.warn", "Couldn't write to '%s'.", destpath);
                        ok = false;
                    }
                }
            } while(n > 0);

            // Finished. Close the files.
            fin.close();
            fout.close();

            if(ok) {
                // Everything went ok. Remove the incomplete file.
                if(remove(m_Download->incompletePath().c_str()) == -1)
                    NNLOG("museekd.down.warn", "Couldn't remove '%s'.", m_Download->incompletePath().c_str());
            }
            else {
                // Things went not ok. Delete the destination file.
                NNLOG("museekd.down.debug", "Removing '%s'.", destpath);
                remove(destpath);
            }
        }
        else {
            // Something happened. But nobody knows what.
            NNLOG("museekd.down.warn", "Renaming '%s' to '%s' failed for unknown reason.", m_Download->incompletePath().c_str(), destpath);
        }
    }
}

/*
    Called when we don't receive any data in this socket
*/
void
Museek::DownloadSocket::dataTimeout(long) {
    NNLOG("museekd.down.debug", "Data timeout while downloading.");
    stop();
}
