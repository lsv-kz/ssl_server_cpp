#include "main.h"

using namespace std;
//======================================================================
#define FCGI_RESPONDER  1

#define FCGI_VERSION_1           1
#define FCGI_BEGIN_REQUEST       1
#define FCGI_ABORT_REQUEST       2
#define FCGI_END_REQUEST         3
#define FCGI_PARAMS              4
#define FCGI_STDIN               5
#define FCGI_STDOUT              6
#define FCGI_STDERR              7
#define FCGI_DATA                8
#define FCGI_GET_VALUES          9
#define FCGI_GET_VALUES_RESULT  10
#define FCGI_UNKNOWN_TYPE       11
#define FCGI_MAXTYPE            (FCGI_UNKNOWN_TYPE)
#define requestId               1
//======================================================================
extern struct pollfd *poll_fd;

int get_sock_fcgi(Connect *r, const char *script);
void del_from_list(Connect *r);
int cgi_set_size_chunk(Connect *r);
int cgi_find_empty_line(Connect *req);
int read_padding(Connect *r);
void read_data_from_buf(Connect *r);
//======================================================================
void fcgi_set_header(Connect* r, int type)
{
    r->fcgi.fcgi_type = type;
    r->fcgi.paddingLen = 0;
    char *p = r->cgi->buf;
    *p++ = FCGI_VERSION_1;
    *p++ = (unsigned char)type;
    *p++ = (unsigned char) ((1 >> 8) & 0xff);
    *p++ = (unsigned char) ((1) & 0xff);
    *p++ = (unsigned char) ((r->fcgi.dataLen >> 8) & 0xff);
    *p++ = (unsigned char) ((r->fcgi.dataLen) & 0xff);
    *p++ = r->fcgi.paddingLen;
    *p = 0;
    
    r->cgi->p = r->cgi->buf;
    r->cgi->len_buf += 8;
}
//======================================================================
void get_info_from_header(Connect* r, const char* p)
{
    r->fcgi.fcgi_type = (unsigned char)p[1];
    r->fcgi.paddingLen = (unsigned char)p[6];
    r->fcgi.dataLen = ((unsigned char)p[4]<<8) | (unsigned char)p[5];
    r->fcgi.len_buf -= 8;
}
//======================================================================
void fcgi_set_param(Connect *r)
{
    r->cgi->len_buf = 0;
    r->cgi->p = r->cgi->buf + 8;

    for ( ; r->fcgi.i_param < r->fcgi.size_par; ++r->fcgi.i_param)
    {
        int len_name = r->fcgi.vPar[r->fcgi.i_param].name.size();
        int len_val = r->fcgi.vPar[r->fcgi.i_param].val.size();
        int len = len_name + len_val;
        len += len_name > 127 ? 4 : 1;
        len += len_val > 127 ? 4 : 1;
        if (len > (r->cgi->size_buf - r->cgi->len_buf))
        {
            break;
        }

        if (len_name < 0x80)
            *(r->cgi->p++) = (unsigned char)len_name;
        else
        {
            *(r->cgi->p++) = (unsigned char)((len_name >> 24) | 0x80);
            *(r->cgi->p++) = (unsigned char)(len_name >> 16);
            *(r->cgi->p++) = (unsigned char)(len_name >> 8);
            *(r->cgi->p++) = (unsigned char)len_name;
        }

        if (len_val < 0x80)
            *(r->cgi->p++) = (unsigned char)len_val;
        else
        {
            *(r->cgi->p++) = (unsigned char)((len_val >> 24) | 0x80);
            *(r->cgi->p++) = (unsigned char)(len_val >> 16);
            *(r->cgi->p++) = (unsigned char)(len_val >> 8);
            *(r->cgi->p++) = (unsigned char)len_val;
        }

        memcpy(r->cgi->p, r->fcgi.vPar[r->fcgi.i_param].name.c_str(), len_name);
        r->cgi->p += len_name;
        if (len_val > 0)
        {
            memcpy(r->cgi->p, r->fcgi.vPar[r->fcgi.i_param].val.c_str(), len_val);
            r->cgi->p += len_val;
        }

        r->cgi->len_buf += len;
    }

    if (r->cgi->len_buf > 0)
    {
        r->fcgi.dataLen = r->cgi->len_buf;
        fcgi_set_header(r, FCGI_PARAMS);
    }
    else
    {
        r->fcgi.dataLen = r->cgi->len_buf;
        fcgi_set_header(r, FCGI_PARAMS);
    }
}
//======================================================================
int fcgi_create_connect(Connect *req)
{
    req->cgi->op.fcgi = FASTCGI_CONNECT;
    req->cgi->dir = TO_CGI;
    if (req->reqMethod == M_POST)
    {
        if (req->req_hd.iReqContentType < 0)
        {
            print_err(req, "<%s:%d> Content-Type \?\n", __func__, __LINE__);
            return -RS400;
        }

        if (req->req_hd.reqContentLength < 0)
        {
            print_err(req, "<%s:%d> 411 Length Required\n", __func__, __LINE__);
            return -RS411;
        }

        if (req->req_hd.reqContentLength > conf->ClientMaxBodySize)
        {
            print_err(req, "<%s:%d> 413 Request entity too large: %lld\n", __func__, __LINE__, req->req_hd.reqContentLength);
            return -RS413;
        }
    }

    if (req->cgi_type == PHPFPM)
        req->fcgi.fd = create_fcgi_socket(req, conf->PathPHP.c_str());
    else if (req->cgi_type == FASTCGI)
        req->fcgi.fd = get_sock_fcgi(req, req->scriptName.c_str());
    else
    {
        print_err(req, "<%s:%d> ? req->scriptType=%d\n", __func__, __LINE__, req->cgi_type);
        return -RS500;
    }

    if (req->fcgi.fd < 0)
    {
        return req->fcgi.fd;
    }

    int i = 0;
    Param param;
    req->fcgi.vPar.clear();

    if (req->cgi_type == PHPFPM)
    {
        param.name = "REDIRECT_STATUS";
        param.val = "true";
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    param.name = "PATH";
    param.val = "/bin:/usr/bin:/usr/local/bin";
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SERVER_SOFTWARE";
    param.val = conf->ServerSoftware;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "GATEWAY_INTERFACE";
    param.val = "CGI/1.1";
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "DOCUMENT_ROOT";
    param.val = conf->DocumentRoot;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REMOTE_ADDR";
    param.val = req->remoteAddr;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REMOTE_PORT";
    param.val = req->remotePort;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REQUEST_URI";
    param.val = req->uri;
    req->fcgi.vPar.push_back(param);
    ++i;
    
    param.name = "DOCUMENT_URI";
    param.val = req->decodeUri;
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "REQUEST_METHOD";
    param.val = get_str_method(req->reqMethod);
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SERVER_PROTOCOL";
    param.val = get_str_http_prot(req->httpProt);
    req->fcgi.vPar.push_back(param);
    ++i;
    
    param.name = "SERVER_PORT";
    param.val = conf->ServerPort;
    req->fcgi.vPar.push_back(param);
    ++i;

    if (req->req_hd.iHost >= 0)
    {
        param.name = "HTTP_HOST";
        param.val = req->reqHdValue[req->req_hd.iHost];
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    if (req->req_hd.iReferer >= 0)
    {
        param.name = "HTTP_REFERER";
        param.val = req->reqHdValue[req->req_hd.iReferer];
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    if (req->req_hd.iUserAgent >= 0)
    {
        param.name = "HTTP_USER_AGENT";
        param.val = req->reqHdValue[req->req_hd.iUserAgent];
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    param.name = "HTTP_CONNECTION";
    if (req->connKeepAlive == 1)
        param.val = "keep-alive";
    else
        param.val = "close";
    req->fcgi.vPar.push_back(param);
    ++i;

    param.name = "SCRIPT_NAME";
    param.val = req->decodeUri;
    req->fcgi.vPar.push_back(param);
    ++i;

    if (req->cgi_type == PHPFPM)
    {
        param.name = "SCRIPT_FILENAME";
        param.val = conf->DocumentRoot + req->scriptName.c_str();
        req->fcgi.vPar.push_back(param);
        ++i;
    }

    if (req->reqMethod == M_POST)
    {
        if (req->req_hd.iReqContentType >= 0)
        {
            param.name = "CONTENT_TYPE";
            param.val = req->reqHdValue[req->req_hd.iReqContentType];
            req->fcgi.vPar.push_back(param);
            ++i;
        }

        if (req->req_hd.iReqContentLength >= 0)
        {
            param.name = "CONTENT_LENGTH";
            param.val = req->reqHdValue[req->req_hd.iReqContentLength];
            req->fcgi.vPar.push_back(param);
            ++i;
        }
    }

    param.name = "QUERY_STRING";
    if (req->sReqParam)
        param.val = req->sReqParam;
    else
        param.val = "";
    req->fcgi.vPar.push_back(param);
    ++i;

    if (i != (int)req->fcgi.vPar.size())
    {
        print_err(req, "<%s:%d> Error: create fcgi param list\n", __func__, __LINE__);
    }

    req->fcgi.size_par = i;
    req->fcgi.i_param = 0;
    //----------------------------------------------
    req->fcgi.dataLen = req->cgi->len_buf = 8;
    fcgi_set_header(req, FCGI_BEGIN_REQUEST);
    char *p = req->cgi->buf + 8;
    *(p++) = (unsigned char) ((FCGI_RESPONDER >> 8) & 0xff);
    *(p++) = (unsigned char) (FCGI_RESPONDER        & 0xff);
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;
    *(p++) = 0;

    req->cgi->op.fcgi = FASTCGI_BEGIN;
    req->cgi->dir = TO_CGI;
    req->sock_timer = 0;
    req->fcgi.http_headers_received = false;

    return 0;
}
//======================================================================
int fcgi_stdin(Connect *r)// return [ ERR_TRY_AGAIN | -1 | 0 ]
{
    if (r->cgi->dir == FROM_CLIENT)
    {
        int rd = (r->cgi->len_post > r->cgi->size_buf) ? r->cgi->size_buf : r->cgi->len_post;
        r->cgi->len_buf = read_from_client(r, r->cgi->buf + 8, rd);
        if (r->cgi->len_buf < 0)
        {
            if (r->cgi->len_buf == ERR_TRY_AGAIN)
                return ERR_TRY_AGAIN;
            return -1;
        }
        else if (r->cgi->len_buf == 0)
        {
            print_err(r, "<%s:%d> Error read()=0\n", __func__, __LINE__);
            return -1;
        }

        r->cgi->len_post -= r->cgi->len_buf;
        r->fcgi.dataLen = r->cgi->len_buf;
        fcgi_set_header(r, FCGI_STDIN);
        r->cgi->dir = TO_CGI;
    }
    else if (r->cgi->dir == TO_CGI)
    {
        if ((r->lenTail > 0) && (r->cgi->len_buf == 0))
        {
            if (r->lenTail > r->cgi->size_buf)
                r->cgi->len_buf = r->cgi->size_buf;
            else
                r->cgi->len_buf = r->lenTail;
            memcpy(r->cgi->buf + 8, r->tail, r->cgi->len_buf);
            r->lenTail -= r->cgi->len_buf;
            r->cgi->len_post -= r->cgi->len_buf;
            if (r->lenTail == 0)
                r->tail = NULL;
            else
                r->tail += r->cgi->len_buf;
            r->fcgi.dataLen = r->cgi->len_buf;
            fcgi_set_header(r, FCGI_STDIN);
        }

        int n = write(r->fcgi.fd, r->cgi->p, r->cgi->len_buf);
        if (n == -1)
        {
            print_err(r, "<%s:%d> Error write(): %s\n", __func__, __LINE__, strerror(errno));
            if (errno == EAGAIN)
                return ERR_TRY_AGAIN;
            return -1;
        }

        r->cgi->p += n;
        r->cgi->len_buf -= n;
        if (r->cgi->len_buf == 0)
        {
            if (r->cgi->len_post <= 0)
            {
                if (r->fcgi.dataLen == 0)
                {
                    r->cgi->op.fcgi = FASTCGI_READ_HTTP_HEADERS;
                    r->fcgi.status = FCGI_READ_HEADER;
                    r->fcgi.len_buf = 0;
                    r->fcgi.ptr_rd = r->fcgi.ptr_wr = r->fcgi.buf;

                    r->p_newline = r->cgi->p = r->cgi->buf + 8;
                    r->cgi->len_buf = 0;
                    r->tail = NULL;
                    
                    r->cgi->dir = FROM_CGI;
                }
                else
                {
                    r->cgi->len_buf = 0;
                    r->fcgi.dataLen = r->cgi->len_buf;
                    fcgi_set_header(r, FCGI_STDIN);  // post data = 0
                    r->cgi->dir = TO_CGI;
                }
            }
            else
            {
                if (r->lenTail > 0)
                {
                    r->cgi->dir = TO_CGI;
                }
                else
                {
                    r->cgi->dir = FROM_CLIENT;
                }
            }
        }
    }

    return 0;
}
//======================================================================
int fcgi_stdout(Connect *r)// return [ ERR_TRY_AGAIN | -1 | 0 | 1 | 0< ]
{
    if (r->cgi->dir == FROM_CGI)
    {
        if ((r->cgi->op.fcgi == FASTCGI_SEND_ENTITY) || 
            (r->cgi->op.fcgi == FASTCGI_READ_ERROR) ||
            (r->cgi->op.fcgi == FASTCGI_CLOSE))
        {
            if (r->fcgi.dataLen == 0)
            {
                r->fcgi.status = FCGI_READ_PADDING;
                return 1;
            }
            
            if (r->fcgi.len_buf > 0)
            {
                read_data_from_buf(r);
                r->cgi->p = r->cgi->buf + 8;
            }
            else
            {
                int len = (r->fcgi.dataLen > r->cgi->size_buf) ? r->cgi->size_buf : r->fcgi.dataLen;
                int ret = read(r->fcgi.fd, r->cgi->buf + 8, len);
                if (ret == -1)
                {
                    print_err(r, "<%s:%d> Error read from script(fd=%d): %s(%d)\n", 
                            __func__, __LINE__, r->fcgi.fd, strerror(errno), errno);
                    if (errno == EAGAIN)
                        return ERR_TRY_AGAIN;
                    else
                    {
                        r->err = -1;
                        return -1;
                    }
                }
                else if (ret == 0)
                {
                    r->err = -1;
                    return -1;
                }

                r->cgi->len_buf = ret;
                r->cgi->p = r->cgi->buf + 8;
                r->fcgi.dataLen -= ret;
            }

            if (r->cgi->op.fcgi == FASTCGI_SEND_ENTITY)
            {
                r->cgi->dir = TO_CLIENT;
                if (r->mode_send == CHUNK)
                {
                    r->cgi->p = r->cgi->buf + 8;
                    if (cgi_set_size_chunk(r))
                        return -1;
                }
                else
                    r->cgi->p = r->cgi->buf + 8;
            }
            else if (r->cgi->op.fcgi == FASTCGI_READ_ERROR)
            {
                if (r->fcgi.dataLen == 0)
                {
                    r->fcgi.status = FCGI_READ_PADDING;
                }
                *(r->cgi->buf + 8 + r->cgi->len_buf) = 0;
                fprintf(stderr, "%s\n", r->cgi->buf + 8);
            }
            else if (r->cgi->op.fcgi == FASTCGI_CLOSE)
            {
                if (r->fcgi.dataLen == 0)
                {
                    if (r->mode_send == NO_CHUNK)
                    {
                        r->connKeepAlive = 0;
                        return 0;
                    }
                    else
                    {
                        r->mode_send = CHUNK_END;
                        r->cgi->len_buf = 0;
                        r->cgi->p = r->cgi->buf + 8;
                        cgi_set_size_chunk(r);
                        r->cgi->dir = TO_CLIENT;
                    }
                }
            }
        }
    }
    else if (r->cgi->dir == TO_CLIENT)
    {
        int ret = write_to_client(r, r->cgi->p, r->cgi->len_buf);
        if (ret < 0)
        {
            if (ret == ERR_TRY_AGAIN)
                return ERR_TRY_AGAIN;
            else
            {
                r->err = -1;
                return -1;
            }
        }

        r->cgi->p += ret;
        r->cgi->len_buf -= ret;
        r->send_bytes += ret;
        if (r->cgi->len_buf == 0)
        {
            if (r->cgi->op.fcgi == FASTCGI_CLOSE)
            {
                return 0;
            }

            if (r->fcgi.dataLen == 0)
            {
                if (r->fcgi.paddingLen == 0)
                {
                    r->fcgi.status = FCGI_READ_HEADER;
                    if (r->fcgi.len_buf <= 0)
                    {
                        r->fcgi.ptr_rd = r->fcgi.ptr_wr = r->fcgi.buf;
                    }

                    r->cgi->p = r->cgi->buf + 8;
                    r->cgi->len_buf = 0;
                }
                else
                {
                    r->fcgi.status = FCGI_READ_PADDING;
                }
            }

            r->cgi->dir = FROM_CGI;
        }
    }
    return 1;
}
//======================================================================
int fcgi_read_http_headers(Connect *r)
{
    if (r->fcgi.len_buf > 0)
    {
        read_data_from_buf(r);
        r->lenTail = r->cgi->len_buf;
    }
    else
    {
        int num_read;
        if ((r->cgi->size_buf - r->cgi->len_buf - 1) >= r->fcgi.dataLen)
            num_read = r->fcgi.dataLen;
        else
            num_read = r->cgi->size_buf - r->cgi->len_buf - 1;
        if (num_read <= 0)
        {
            r->err = -RS502;
            return -1;
        }

        int n = read(r->fcgi.fd, r->cgi->p, num_read);
        if (n == -1)
        {
            print_err(r, "<%s:%d> Error read(): %s\n", __func__, __LINE__, strerror(errno));
            if (errno == EAGAIN)
                return ERR_TRY_AGAIN;
            else
            {
                r->err = -RS502;
                return -1;
            }
        }
        else if (n == 0)
        {
            r->err = -RS502;
            return -1;
        }

        r->fcgi.dataLen -= n;
        r->lenTail += n;
        r->cgi->len_buf += n;
        r->cgi->p += n;
    }
    
    *(r->cgi->p) = 0;
    
    int ret = cgi_find_empty_line(r);
    if (ret == 1) // empty line found
    {
        return r->cgi->len_buf;
    }
    else if (ret < 0) // error
    {
        r->err = -RS502;
        return -1;
    }

    return 0;
}
//======================================================================
int write_to_fcgi(Connect* r)
{
    int ret = write(r->fcgi.fd, r->cgi->p, r->cgi->len_buf);
    if (ret == -1)
    {
        if (errno == EAGAIN)
            return ERR_TRY_AGAIN;
        else
        {
            print_err(r, "<%s:%d> Error write to fcgi: %s\n", __func__, __LINE__, strerror(errno));
            return -1;
        }
    }
    else
    {
        r->cgi->len_buf -= ret;
        r->cgi->p += ret;
    }
    
    return ret;
}
//======================================================================
int fcgi_read_header(Connect* r)
{
    int n = 0;
    if (r->fcgi.len_buf >= 8)
    {
        get_info_from_header(r, r->fcgi.ptr_rd);
        r->fcgi.ptr_rd += 8;
        return 8;
    }
    else
    {
        int len = r->fcgi.size_buf - r->fcgi.len_buf;
        n = read(r->fcgi.fd, r->fcgi.ptr_wr, len);
        if (n == -1)
        {
            if (errno == EAGAIN)
                return ERR_TRY_AGAIN;
            print_err(r, "<%s:%d> Error fcgi_read_header(): %s\n", __func__, __LINE__, strerror(errno));
            return -1;
        }
        else if (n == 0)
        {
            print_err(r, "<%s:%d> Error read from fcgi: read()=0, len=%d\n", __func__, __LINE__, len);
            return -1;
        }

        r->fcgi.len_buf += n;
        r->fcgi.ptr_wr += n;

        if (r->fcgi.len_buf >= 8)
        {
            get_info_from_header(r, r->fcgi.ptr_rd);
            r->fcgi.ptr_rd += 8;
            return 8;
        }
    }

    return 0;
}
//======================================================================
void fcgi_set_poll_list(Connect *r, int *i)
{
    if (r->cgi->dir == FROM_CLIENT)
    {
        r->timeout = conf->Timeout;
        poll_fd[*i].fd = r->clientSocket;
        poll_fd[*i].events = POLLIN;
    }
    else if (r->cgi->dir == TO_CLIENT)
    {
        r->timeout = conf->Timeout;
        poll_fd[*i].fd = r->clientSocket;
        poll_fd[*i].events = POLLOUT;
    }
    else if (r->cgi->dir == FROM_CGI)
    {
        r->timeout = conf->TimeoutCGI;
        poll_fd[*i].fd = r->fcgi.fd;
        poll_fd[*i].events = POLLIN;
    }
    else if (r->cgi->dir == TO_CGI)
    {
        r->timeout = conf->TimeoutCGI;
        poll_fd[*i].fd = r->fcgi.fd;
        poll_fd[*i].events = POLLOUT;
    }

    (*i)++;
}
//======================================================================
void fcgi_worker(Connect* r)
{
    if (r->cgi->op.fcgi == FASTCGI_BEGIN)
    {
        int ret = write_to_fcgi(r);
        if (ret > 0)
        {
            r->sock_timer = 0;
            if (r->cgi->len_buf == 0)
            {
                r->cgi->op.fcgi = FASTCGI_PARAMS;
                r->cgi->dir = TO_CGI;
            }
        }
        else if (ret < 0)
        {
            if (ret == ERR_TRY_AGAIN)
                r->io_status = POLL;
            else
            {
                r->err = -RS502;
                del_from_list(r);
                end_response(r);
            }
        }
    }
    else if (r->cgi->op.fcgi == FASTCGI_PARAMS)
    {
        if (r->cgi->len_buf == 0)
        {
            fcgi_set_param(r);
        }

        int ret = write_to_fcgi(r);
        if (ret > 0)
        {
            r->sock_timer = 0;
            if ((r->cgi->len_buf == 0) && (r->fcgi.dataLen == 0)) // end params
            {
                r->cgi->op.fcgi = FASTCGI_STDIN;
                if (r->req_hd.reqContentLength > 0)
                {
                    r->cgi->len_post = r->req_hd.reqContentLength;
                    r->sock_timer = 0;
                    if (r->lenTail > 0)
                    {
                        r->cgi->len_buf = 0;
                        r->cgi->dir = TO_CGI;
                    }
                    else
                    {
                        r->cgi->dir = FROM_CLIENT;
                    }
                }
                else
                {
                    r->cgi->len_post = 0;
                    r->cgi->len_buf = 0;
                    r->fcgi.dataLen = r->cgi->len_buf;
                    fcgi_set_header(r, FCGI_STDIN);  // post data = 0
                    r->sock_timer = 0;
                    r->cgi->dir = TO_CGI;
                }
            }
        }
        else if (ret < 0)
        {
            if (ret == ERR_TRY_AGAIN)
                r->io_status = POLL;
            else
            {
                r->err = -RS502;
                del_from_list(r);
                end_response(r);
            }
        }
    }
    else if (r->cgi->op.fcgi == FASTCGI_STDIN)
    {
        int ret = fcgi_stdin(r);
        if (ret < 0)
        {
            if (ret == ERR_TRY_AGAIN)
                r->io_status = POLL;
            else
            {
                r->err = -RS502;
                del_from_list(r);
                end_response(r);
            }
        }
        else
            r->sock_timer = 0;
    }
    else//====================== FCGI_STDOUT============================
    {
        if (r->fcgi.status == FCGI_READ_HEADER)
        {
            int ret = fcgi_read_header(r);
            if (ret < 0)
            {
                if (ret == ERR_TRY_AGAIN)
                    r->io_status = POLL;
                else
                {
                    if (r->cgi->op.fcgi <= FASTCGI_READ_HTTP_HEADERS)
                        r->err = -RS502;
                    else
                        r->err = -1;
                    del_from_list(r);
                    end_response(r);
                }
            }
            else if (ret < 8)
                r->sock_timer = 0;
            else if (ret >= 8)
            {
                r->sock_timer = 0;
                r->fcgi.status = FCGI_READ_DATA;
                switch (r->fcgi.fcgi_type)
                {
                    case FCGI_STDOUT:
                        if (r->fcgi.dataLen == 0)
                        {
                            r->fcgi.status = FCGI_READ_HEADER;
                            if (r->fcgi.len_buf <= 0)
                            {
                                r->fcgi.ptr_rd = r->fcgi.ptr_wr = r->fcgi.buf;
                            }

                            r->cgi->p = r->cgi->buf + 8;
                            r->cgi->len_buf = 0;
                        }
                        else
                        {
                            if (r->fcgi.http_headers_received == true)
                                r->cgi->op.fcgi = FASTCGI_SEND_ENTITY;
                            else
                                r->cgi->op.fcgi = FASTCGI_READ_HTTP_HEADERS;
                        }

                        r->cgi->dir = FROM_CGI;
                        break;
                    case FCGI_STDERR:
                        r->cgi->op.fcgi = FASTCGI_READ_ERROR;
                        r->cgi->dir = FROM_CGI;
                        break;
                    case FCGI_END_REQUEST:
                        r->cgi->op.fcgi = FASTCGI_CLOSE;
                        if (r->fcgi.len_buf < r->fcgi.dataLen)
                        {
                             r->fcgi.dataLen -= r->fcgi.len_buf;
                             r->fcgi.len_buf = 0;
                             r->cgi->dir = FROM_CGI;
                        }
                        else
                        {
                            if (r->mode_send == NO_CHUNK)
                            {
                                r->connKeepAlive = 0;
                                del_from_list(r);
                                end_response(r);
                            }
                            else
                            {
                                r->fcgi.dataLen = 0;
                                r->mode_send = CHUNK_END;
                                r->cgi->len_buf = 0;
                                r->cgi->p = r->cgi->buf + 8;
                                cgi_set_size_chunk(r);
                                r->cgi->dir = TO_CLIENT;
                            }
                        }
                        break;
                    default:
                        print_err(r, "<%s:%d> Error type=%d\n", __func__, __LINE__, r->fcgi.fcgi_type);
                        r->err = -1;
                        del_from_list(r);
                        end_response(r);
                }
            }
            return;
        }
        else if (r->fcgi.status == FCGI_READ_PADDING)
        {
            int ret = read_padding(r);
            if (ret < 0)
            {
                if (ret == ERR_TRY_AGAIN)
                    r->io_status = POLL;
                else
                {
                    r->err = -1;
                    del_from_list(r);
                    end_response(r);
                }
            }
            return;
        }

        if (r->cgi->op.fcgi == FASTCGI_READ_HTTP_HEADERS)
        {
            int ret = fcgi_read_http_headers(r);
            if (ret < 0)
            {
                if (ret == ERR_TRY_AGAIN)
                    r->io_status = POLL;
                else
                {
                    r->err = -RS502;
                    del_from_list(r);
                    end_response(r);
                }
            }
            else if (ret > 0)
            {
                r->mode_send = ((r->httpProt == HTTP11) && r->connKeepAlive) ? CHUNK : NO_CHUNK;
                if (create_response_headers(r))
                {
                    print_err(r, "<%s:%d> Error create_response_headers()\n", __func__, __LINE__);
                    r->err = -1;
                    del_from_list(r);
                    end_response(r);
                }
                else
                {
                    r->fcgi.http_headers_received = true;
                    r->resp_headers.p = r->resp_headers.s.c_str();
                    r->resp_headers.len = r->resp_headers.s.size();
                    r->cgi->op.fcgi = FASTCGI_SEND_HTTP_HEADERS;
                    r->cgi->dir = TO_CLIENT;
                    r->sock_timer = 0;
                }
            }
            else
            {
                r->sock_timer = 0;
                if (r->fcgi.dataLen == 0)
                {
                    if (r->fcgi.paddingLen == 0)
                    {
                        r->fcgi.status = FCGI_READ_HEADER;
                        if (r->fcgi.len_buf <= 0)
                        {
                            r->fcgi.ptr_rd = r->fcgi.ptr_wr = r->fcgi.buf;
                        }

                        r->cgi->p = r->cgi->buf + 8;
                        r->cgi->len_buf = 0;
                        r->cgi->dir = FROM_CGI;
                    }
                    else
                    {
                        r->fcgi.status = FCGI_READ_PADDING;
                        r->cgi->dir = FROM_CGI;
                    }
                }
            }
        }
        else if (r->cgi->op.fcgi == FASTCGI_SEND_HTTP_HEADERS)
        {
            if (r->resp_headers.len > 0)
            {
                int wr = write_to_client(r, r->resp_headers.p, r->resp_headers.len);
                if (wr < 0)
                {
                    if (wr == ERR_TRY_AGAIN)
                        r->io_status = POLL;
                    else
                    {
                        r->err = -1;
                        r->req_hd.iReferer = MAX_HEADERS - 1;
                        r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                        del_from_list(r);
                        end_response(r);
                    }
                }
                else
                {
                    r->resp_headers.p += wr;
                    r->resp_headers.len -= wr;
                    if (r->resp_headers.len == 0)
                    {
                        if (r->reqMethod == M_HEAD)
                        {
                            del_from_list(r);
                            end_response(r);
                        }
                        else
                        {
                            r->cgi->op.fcgi = FASTCGI_SEND_ENTITY;
                            r->sock_timer = 0;
                            if (r->lenTail > 0)
                            {
                                r->cgi->p = r->tail;
                                r->cgi->len_buf = r->lenTail;
                                r->tail = NULL;
                                r->lenTail = 0;
                                
                                r->cgi->dir = TO_CLIENT;
                                r->sock_timer = 0;
                                if (r->mode_send == CHUNK)
                                {
                                    if (cgi_set_size_chunk(r))
                                    {
                                        r->err = -1;
                                        del_from_list(r);
                                        end_response(r);
                                    }
                                }
                            }
                            else
                            {
                                r->cgi->dir = FROM_CGI;
                                r->sock_timer = 0;
                            }
                        }
                    }
                    else
                        r->sock_timer = 0;
                }
            }
        }
        else if ((r->cgi->op.fcgi == FASTCGI_SEND_ENTITY) ||
                 (r->cgi->op.fcgi == FASTCGI_READ_ERROR) ||
                 (r->cgi->op.fcgi == FASTCGI_CLOSE))
        {
            int ret = fcgi_stdout(r);
            if (ret < 0)
            {
                if (ret == ERR_TRY_AGAIN)
                    r->io_status = POLL;
                else
                {
                    r->err = -1;
                    del_from_list(r);
                    end_response(r);
                }
            }
            else if (ret == 0)
            {
                del_from_list(r);
                end_response(r);
            }
            else
                r->sock_timer = 0;
        }
        else
        {
            print_err(r, "<%s:%d> ??? Error: FCGI_OPERATION=%s\n", __func__, __LINE__, get_fcgi_operation(r->cgi->op.fcgi));
            r->err = -1;
            del_from_list(r);
            end_response(r);
        }
    }
}
//======================================================================
int timeout_fcgi(Connect *r)
{
    if (((r->cgi->op.fcgi == FASTCGI_BEGIN) || 
         (r->cgi->op.fcgi == FASTCGI_PARAMS) || 
         (r->cgi->op.fcgi == FASTCGI_STDIN)) && 
        (r->cgi->dir == TO_CGI))
        return -RS504;
    else if (r->cgi->op.fcgi == FASTCGI_READ_HTTP_HEADERS)
        return -RS504;
    else
        return -1;
}
//======================================================================
int read_padding(Connect *r)
{
    if (r->fcgi.paddingLen > 0)
    {
        if (r->fcgi.len_buf > 0)
        {
            r->sock_timer = 0;
            r->cgi->dir = FROM_CGI;
            if (r->fcgi.len_buf >= r->fcgi.paddingLen)
            {
                r->fcgi.len_buf -= r->fcgi.paddingLen;
                r->fcgi.ptr_rd += r->fcgi.paddingLen;
                r->fcgi.paddingLen = 0;
                r->fcgi.status = FCGI_READ_HEADER;
                if (r->fcgi.len_buf <= 0)
                {
                    r->fcgi.ptr_rd = r->fcgi.ptr_wr = r->fcgi.buf;
                }
                r->cgi->p = r->cgi->buf + 8;
                r->cgi->len_buf = 0;
            }
            else
            {
                r->fcgi.paddingLen -= r->fcgi.len_buf;
                r->fcgi.len_buf = 0;
            }
            return 0;
        }

        char buf[256];

        int len = (r->fcgi.paddingLen > (int)sizeof(buf)) ? sizeof(buf) : r->fcgi.paddingLen;
        int n = read(r->fcgi.fd, buf, len);
        if (n == -1)
        {
            print_err(r, "<%s:%d> Error read from script(fd=%d): %s\n", 
                    __func__, __LINE__, r->fcgi.fd, strerror(errno));
            if (errno == EAGAIN)
                return ERR_TRY_AGAIN;
            else
            {
                r->err = -1;
                return -1;
            }
        }
        else if (n == 0)
        {
            r->err = -1;
            return -1;
        }
        else
        {
            r->fcgi.paddingLen -= n;
        }
    }

    if (r->fcgi.paddingLen == 0)
    {
        r->sock_timer = 0;
        r->fcgi.status = FCGI_READ_HEADER;
        if (r->fcgi.len_buf <= 0)
        {
            r->fcgi.ptr_rd = r->fcgi.ptr_wr = r->fcgi.buf;
        }
        r->cgi->p = r->cgi->buf + 8;
        r->cgi->len_buf = 0;
        r->cgi->dir = FROM_CGI;
    }

    return 0;
}
//======================================================================
void read_data_from_buf(Connect *r)
{
    r->cgi->p = r->cgi->buf + 8;
    if (r->fcgi.len_buf <= r->fcgi.dataLen)
    {
        memcpy(r->cgi->p, r->fcgi.ptr_rd, r->fcgi.len_buf);
        r->cgi->len_buf = r->fcgi.len_buf;
        r->cgi->p += r->fcgi.len_buf;

        r->fcgi.ptr_rd = r->fcgi.buf;
        r->fcgi.dataLen -= r->fcgi.len_buf;
        r->fcgi.len_buf = 0;
    }
    else if (r->fcgi.len_buf >= r->fcgi.dataLen)
    {
        memcpy(r->cgi->p, r->fcgi.ptr_rd, r->fcgi.dataLen);
        r->cgi->len_buf = r->fcgi.dataLen;
        r->cgi->p += r->fcgi.dataLen;
        
        r->fcgi.ptr_rd += r->fcgi.dataLen;
        r->fcgi.len_buf -= r->fcgi.dataLen;
        r->fcgi.dataLen -= r->fcgi.dataLen;
    }
}
