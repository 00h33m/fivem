/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "HttpServer.h"
#include "HttpServerImpl.h"

#include <nghttp2/nghttp2.h>

#include <deque>

struct ZeroCopyByteBuffer
{
	struct Element
	{
		std::string string;
		std::vector<uint8_t> vec;
		std::unique_ptr<char[]> raw;
		size_t rawLength;
		size_t read;

		int type;

		Element(std::vector<uint8_t>&& vec)
			: read(0), type(1), vec(std::move(vec))
		{
			
		}

		Element(std::string&& str)
			: read(0), type(0), string(std::move(str))
		{
			
		}

		Element(std::unique_ptr<char[]> raw, size_t length)
			: read(0), type(2), raw(std::move(raw)), rawLength(length)
		{

		}

		// we lied! we copy anyway :(
		Element(const std::vector<uint8_t>& vec)
			: read(0), type(1), vec(vec)
		{
			this->vec = vec;
		}

		Element(const std::string& str)
			: read(0), type(0), string(str)
		{
			
		}

		size_t Size() const
		{
			switch (type)
			{
			case 0:
				return string.size();
			case 1:
				return vec.size();
			case 2:
				return rawLength;
			}

			return 0;
		}
	};

	template<typename TContainer>
	void Push(TContainer&& elem)
	{
		elements.emplace_back(std::move(elem));
	}

	void Push(std::unique_ptr<char[]> data, size_t size)
	{
		elements.emplace_back(std::move(data), size);
	}

	bool Pop(const std::string** str, const std::vector<uint8_t>** vec, size_t* size, size_t* off)
	{
		if (elements.empty())
		{
			return false;
		}

		const auto& elem = elements.front();
		*off = elem.read;

		switch (elem.type)
		{
		case 0:
			*str = &elem.string;
			*vec = nullptr;
			break;
		case 1:
			*vec = &elem.vec;
			*str = nullptr;
			break;
		case 2:
			*vec = nullptr;
			*str = nullptr;
			*size = elem.rawLength;
			break;
		}

		return true;
	}

	ssize_t PeekLength()
	{
		const std::string* s;
		const std::vector<uint8_t>* v;
		size_t size = 0;
		size_t off = 0;

		if (Pop(&s, &v, &size, &off))
		{
			if (s)
			{
				return s->size() - off;
			}
			else if (v)
			{
				return v->size() - off;
			}
			else if (size)
			{
				return size - off;
			}
		}

		return -1;
	}

	bool Take(std::string* str, std::vector<uint8_t>* vec, std::unique_ptr<char[]>* raw, size_t* rawLength, size_t* off)
	{
		if (elements.empty())
		{
			return false;
		}

		{
			auto& elem = elements.front();
			*off = elem.read;

			switch (elem.type)
			{
			case 0:
				*str = std::move(elem.string);
				break;
			case 1:
				*vec = std::move(elem.vec);
				break;
			case 2:
				*raw = std::move(elem.raw);
				*rawLength = std::move(elem.rawLength);
				break;
			}
		}

		elements.pop_front();

		return true;
	}

	bool Has(size_t len)
	{
		size_t accounted = 0;

		for (const auto& elem : elements)
		{
			auto thisLen = std::min(len - accounted, elem.Size() - elem.read);
			accounted += thisLen;

			if (accounted > len)
			{
				return true;
			}
		}

		return (len <= accounted);
	}

	void Advance(size_t len)
	{
		while (len > 0)
		{
			auto& elem = elements.front();
			size_t s = elem.Size();

			auto thisLen = std::min(len, s - elem.read);
			elem.read += thisLen;
			len -= thisLen;

			if (elem.read >= s)
			{
				elements.pop_front();
			}
		}
	}

	bool Empty()
	{
		return elements.empty();
	}

private:
	std::deque<Element> elements;
};

namespace net
{
class Http2Response : public HttpResponse
{
public:
	inline Http2Response(fwRefContainer<HttpRequest> request, nghttp2_session* session, int streamID)
		: HttpResponse(request), m_session(session), m_stream(streamID)
	{

	}

	virtual ~Http2Response()
	{

	}

	virtual void WriteHead(int statusCode, const std::string& statusMessage, const HeaderMap& headers) override
	{
		if (m_sentHeaders)
		{
			return;
		}

		if (!m_session)
		{
			return;
		}

		m_headers = headers;
		m_headers.insert({ ":status", std::to_string(statusCode) });

		for (auto& header : m_headerList)
		{
			m_headers.insert(header);
		}

		// don't have transfer_encoding at all!
		m_headers.erase("transfer-encoding");

		nghttp2_data_provider provider;
		provider.source.ptr = this;
		provider.read_callback = [](nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data) -> ssize_t
		{
			auto resp = reinterpret_cast<Http2Response*>(source->ptr);

			if (resp->m_ended)
			{
				*data_flags = NGHTTP2_DATA_FLAG_EOF;
			}

			if (resp->m_buffer.Empty())
			{
				return (resp->m_ended) ? 0 : NGHTTP2_ERR_DEFERRED;
			}

			*data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;
			return resp->m_buffer.PeekLength();
		};

		std::vector<nghttp2_nv> nv(m_headers.size());

		size_t i = 0;
		for (auto& hdr : m_headers)
		{
			auto& v = nv[i];

			v.flags = 0;
			v.name = (uint8_t*)hdr.first.c_str();
			v.namelen = hdr.first.length();
			v.value = (uint8_t*)hdr.second.c_str();
			v.valuelen = hdr.second.length();

			++i;
		}

		nghttp2_submit_response(m_session, m_stream, nv.data(), nv.size(), &provider);
		nghttp2_session_send(m_session);

		m_sentHeaders = true;
	}

	template<typename TContainer>
	void WriteOutInternal(TContainer data)
	{
		if (m_session)
		{
			m_buffer.Push(std::forward<TContainer>(data));

			nghttp2_session_resume_data(m_session, m_stream);
			nghttp2_session_send(m_session);
		}
	}

	virtual void WriteOut(const std::vector<uint8_t>& data) override
	{
		WriteOutInternal<decltype(data)>(data);
	}

	virtual void WriteOut(std::vector<uint8_t>&& data) override
	{
		WriteOutInternal<decltype(data)>(std::move(data));
	}

	virtual void WriteOut(const std::string& data) override
	{
		WriteOutInternal<decltype(data)>(data);
	}

	virtual void WriteOut(std::string&& data) override
	{
		WriteOutInternal<decltype(data)>(std::move(data));
	}

	virtual void WriteOut(std::unique_ptr<char[]> data, size_t size) override
	{
		if (m_session)
		{
			m_buffer.Push(std::move(data), size);

			nghttp2_session_resume_data(m_session, m_stream);
			nghttp2_session_send(m_session);
		}
	}

	virtual void End() override
	{
		m_ended = true;

		if (m_session)
		{
			nghttp2_session_resume_data(m_session, m_stream);
			nghttp2_session_send(m_session);
		}
	}

	void Cancel()
	{
		if (m_request.GetRef() && !m_ended)
		{
			auto cancelHandler = m_request->GetCancelHandler();

			if (cancelHandler)
			{
				(*cancelHandler)();

				m_request->SetCancelHandler();
			}
		}

		m_session = nullptr;
	}

	inline ZeroCopyByteBuffer& GetBuffer()
	{
		return m_buffer;
	}

private:
	nghttp2_session* m_session;

	int m_stream;

	HeaderMap m_headers;

	ZeroCopyByteBuffer m_buffer;
};

Http2ServerImpl::Http2ServerImpl()
{

}

Http2ServerImpl::~Http2ServerImpl()
{

}

void Http2ServerImpl::OnConnection(fwRefContainer<TcpServerStream> stream)
{
	struct HttpRequestData;

	struct HttpConnectionData
	{
		nghttp2_session* session;

		fwRefContainer<TcpServerStream> stream;

		Http2ServerImpl* server;

		std::set<HttpRequestData*> streams;

		// cache responses independently from streams so 'clean' closes don't invalidate session
		std::list<fwRefContainer<HttpResponse>> responses;
	};

	struct HttpRequestData
	{
		HttpConnectionData* connection;

		std::map<std::string, std::string> headers;

		std::vector<uint8_t> body;

		fwRefContainer<HttpRequest> httpReq;

		fwRefContainer<HttpResponse> httpResp;
	};

	nghttp2_session_callbacks* callbacks;
	nghttp2_session_callbacks_new(&callbacks);

	nghttp2_session_callbacks_set_send_callback(callbacks, [](nghttp2_session *session, const uint8_t *data,
		size_t length, int flags, void *user_data) -> ssize_t
	{
		reinterpret_cast<HttpConnectionData*>(user_data)->stream->Write(std::vector<uint8_t>{ data, data + length });

		return length;
	});

	nghttp2_session_callbacks_set_send_data_callback(callbacks, [](nghttp2_session* session, nghttp2_frame* frame, const uint8_t* framehd, size_t length, nghttp2_data_source* source, void* user_data) -> int
	{
		auto data = reinterpret_cast<HttpConnectionData*>(user_data);
		auto resp = reinterpret_cast<Http2Response*>(source->ptr);

		auto& buf = resp->GetBuffer();

		if (buf.Has(length))
		{
			static thread_local std::vector<uint8_t> fhd(9);
			memcpy(&fhd[0], framehd, fhd.size());

			data->stream->Write(fhd);

			std::vector<uint8_t> v;
			std::string s;
			std::unique_ptr<char[]> raw;
			size_t rawLength;
			size_t off;

			if (buf.Take(&s, &v, &raw, &rawLength, &off))
			{
				assert(off == 0);

				if (!s.empty())
				{
					data->stream->Write(std::move(s));
				}
				else if (!v.empty())
				{
					data->stream->Write(std::move(v));
				}
				else if (raw)
				{
					data->stream->Write(std::move(raw), rawLength);
				}
			}

			return 0;
		}

		return NGHTTP2_ERR_WOULDBLOCK;
	});

	nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, [](nghttp2_session *session,
		const nghttp2_frame *frame,
		void *user_data)
	{
		auto conn = reinterpret_cast<HttpConnectionData*>(user_data);

		if (frame->hd.type != NGHTTP2_HEADERS ||
			frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
			return 0;
		}

		auto reqData = new HttpRequestData;
		reqData->connection = conn;
		reqData->httpReq = nullptr;
		reqData->httpResp = nullptr;

		conn->streams.insert(reqData);

		nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, reqData);

		return 0;
	});

	nghttp2_session_callbacks_set_on_header_callback(callbacks, [](nghttp2_session *session,
		const nghttp2_frame *frame, const uint8_t *name,
		size_t namelen, const uint8_t *value,
		size_t valuelen, uint8_t flags,
		void *user_data) -> int
	{
		auto conn = reinterpret_cast<HttpConnectionData*>(user_data);

		switch (frame->hd.type)
		{
			case NGHTTP2_HEADERS:
				if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
					break;
				}

				auto req = reinterpret_cast<HttpRequestData*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				
				req->headers.insert({ {reinterpret_cast<const char*>(name), namelen}, { reinterpret_cast<const char*>(value), valuelen} });

				break;
		}

		return 0;
	});

	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, [](nghttp2_session *session, uint8_t flags,
		int32_t stream_id, const uint8_t *data,
		size_t len, void *user_data)
	{
		auto req = reinterpret_cast<HttpRequestData*>(nghttp2_session_get_stream_user_data(session, stream_id));

		if (req)
		{
			size_t origSize = req->body.size();

			req->body.resize(origSize + len);
			memcpy(&req->body[origSize], data, len);
		}

		return 0;
	});

	nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, [](nghttp2_session *session,
		const nghttp2_frame *frame, void *user_data)
	{
		switch (frame->hd.type) {
			case NGHTTP2_HEADERS:
			{
				auto req = reinterpret_cast<HttpRequestData*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				std::shared_ptr<HttpState> reqState = std::make_shared<HttpState>();

				if (req)
				{
					HeaderMap headerList;

					for (auto& header : req->headers)
					{
						if (header.first[0] != ':')
						{
							headerList.insert(header);
						}
						else if (header.first == ":authority")
						{
							headerList.emplace("host", header.second);
						}
					}

					fwRefContainer<HttpRequest> request = new HttpRequest(2, 0, req->headers[":method"], req->headers[":path"], headerList, req->connection->stream->GetPeerAddress().ToString());
					
					fwRefContainer<HttpResponse> response = new Http2Response(request, session, frame->hd.stream_id);
					req->connection->responses.push_back(response);

					req->httpResp = response;
					reqState->blocked = true;

					for (auto& handler : req->connection->server->m_handlers)
					{
						if (handler->HandleRequest(request, response) || response->HasEnded())
						{
							break;
						}
					}

					if (!response->HasEnded())
					{
						req->httpReq = request;
					}
				}
			}
			break;
			case NGHTTP2_DATA:
				if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
				{
					auto req = reinterpret_cast<HttpRequestData*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));

					if (req->httpReq.GetRef())
					{
						auto handler = req->httpReq->GetDataHandler();
						req->httpReq->SetDataHandler();

						if (handler)
						{
							(*handler)(req->body);
						}
					}
				}

				break;
		}

		return 0;
	});

	nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, [](nghttp2_session *session, int32_t stream_id,
		uint32_t error_code, void *user_data)
	{
		auto req = reinterpret_cast<HttpRequestData*>(nghttp2_session_get_stream_user_data(session, stream_id));

		auto resp = req->httpResp.GetRef();

		if (resp)
		{
			((net::Http2Response*)resp)->Cancel();
		}

		req->connection->streams.erase(req);
		delete req;

		return 0;
	});

	// create a server
	auto data = new HttpConnectionData;
	data->stream = stream;
	data->server = this;

	nghttp2_session_server_new(&data->session, callbacks, data);

	// clean callbacks
	nghttp2_session_callbacks_del(callbacks);

	// send settings
	nghttp2_settings_entry iv[1] = {
		{ NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 } };

	nghttp2_submit_settings(data->session, NGHTTP2_FLAG_NONE, iv, 1);

	//nghttp2_session_send(data->session);

	// handle receive
	stream->SetReadCallback([=](const std::vector<uint8_t>& bytes)
	{
		nghttp2_session_mem_recv(data->session, bytes.data(), bytes.size());

		nghttp2_session_send(data->session);
	});

	stream->SetCloseCallback([=]()
	{
		{
			for (auto& response : data->responses)
			{
				// cancel HTTP responses that we have referenced
				// (so that we won't reference data->session)
				auto resp = response.GetRef();

				if (resp)
				{
					((net::Http2Response*)resp)->Cancel();
				}
			}

			data->responses.clear();
		}

		// free any leftover stream data
		for (auto& stream : data->streams)
		{
			delete stream;
		}

		// delete the session, and bye data
		nghttp2_session_del(data->session);
		delete data;
	});


}
}
