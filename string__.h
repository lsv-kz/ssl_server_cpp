#ifndef MYSTRING_H_
#define MYSTRING_H_

#include <iostream>
#include <cstring>

//======================================================================
class String : public std::string
{
public:
    String& operator >> (double&) = delete;
    String& operator << (double f) = delete;
    String(){}
    explicit String(int n) { if (n == 0) return; reserve(n); }
    String(const char* s):std::string(s) {}//String(const char* s){*this = string(s);}
    String(std::string s):std::string(s) {}
    //~String(){}
    //------------------------------------------------------------------
    String  &operator << (String& s)
    {
        *this += s;
        return *this;
    }
    //------------------------------------------------------------------
    String  &operator << (const std::string& s)
    {
        *this += s;
        return *this;
    }
    //------------------------------------------------------------------
    String  &operator << (char* s)
    {
        *this += s;
        return *this;
    }
    //------------------------------------------------------------------
    String  &operator << (const char* s)
    {
        *this += s;
        return *this;
    }
    //------------------------------------------------------------------
    String  &operator << (const char s)
    {
        *this += s;
        return *this;
    }
    //------------------------------------------------------------------
    template <typename T>
    String & operator << (T t)
    {
        const unsigned long size = 21;
        char s[size];
        int cnt, minus = 0;
        const char *byte_to_char = "9876543210123456789";

        cnt = size - 1;
        if (t < 0) minus = 1;

        s[cnt] = 0;
        while (cnt > 0)
        {
            --cnt;
            s[cnt] = byte_to_char[9 + (t % 10)];
            t /= 10;
            if (t == 0) break;
        }

        if (cnt <= 0)
        {
            return *this;
        }
        if (minus) s[--cnt] = '-';

        *this += (s + cnt);
        return *this;
    }
    //------------------------------------------------------------------
    String & operator >> (String &str)
    {
        int len = this->size(), i;
        for (i = 0; i < len; ++i)
            if (((*this)[i] != ' ') && ((*this)[i] != '\t')) break;
        int j = i;
        for (; j < len; ++j)
            if (((*this)[j] == ' ') || ((*this)[j] == '\t')) break;
        str = substr(i, j - i);
        *this = substr(j);
        return *this;
    }
    //------------------------------------------------------------------
    String & operator >> (std::string &str)
    {
        int len = this->size(), i;
        for (i = 0; i < len; ++i)
            if (((*this)[i] != ' ') && ((*this)[i] != '\t')) break;
        int j = i;
        for (; j < len; ++j)
            if (((*this)[j] == ' ') || ((*this)[j] == '\t')) break;
        str = substr(i, j - i);
        *this = substr(j);
        return *this;
    }
    //------------------------------------------------------------------
    String & operator >> (int &d)
    {
        std::string str;
        int len = this->size(), i;
        for (i = 0; i < len; ++i)
            if (((*this)[i] != ' ') && ((*this)[i] != '\t')) break;
        int j = i;
        if (((*this)[j] == '-') || ((*this)[j] == '+'))
            ++j;
        for (; j < len; ++j)
            if (!isdigit((*this)[j]))
                break;
        str = substr(i, j - i);
        try
        {
            d = stoi(str);
        }
        catch (...)
        {
            fprintf(stderr, "<%s:%d> Error stoi(%s)\n", __func__, __LINE__, str.c_str());
        }
        *this = substr(j);
        return *this;
    }
    //------------------------------------------------------------------
    String & operator >> (unsigned int &d)
    {
        std::string str;
        int len = this->size(), i;
        for (i = 0; i < len; ++i)
            if (((*this)[i] != ' ') && ((*this)[i] != '\t')) break;
        int j = i;
        if ((*this)[j] == '-')
        {
            fprintf(stderr, "<%s:%d> Error: integer is signed\n", __func__, __LINE__);
            return *this;
        }

        if ((*this)[j] == '+')
            ++j;
        for (; j < len; ++j)
            if (!isdigit((*this)[j]))
                break;
        str = substr(i, j - i);
        try
        {
            d = stoi(str);
        }
        catch (...)
        {
            fprintf(stderr, "<%s:%d> Error stoi(%s)\n", __func__, __LINE__, str.c_str());
        }
        *this = substr(j);
        return *this;
    }
    //------------------------------------------------------------------
    String & operator >> (long long &ll)
    {
        std::string str;
        int len = this->size(), i;
        for (i = 0; i < len; ++i)
            if (((*this)[i] != ' ') && ((*this)[i] != '\t')) break;
        int j = i;
        for (; j < len; ++j)
            if (((*this)[j] == ' ') || ((*this)[j] == '\t')) break;
        str = substr(i, j - i);
        try
        {
            ll = stoll(str);
        }
        catch (...)
        {
            fprintf(stderr, "<%s:%d> Error stoll(%s)\n", __func__, __LINE__, str.c_str());
        }
        *this = substr(j);
        return *this;
    }
    //------------------------------------------------------------------
    String & operator >> (long &l)
    {
        std::string str;
        int len = this->size(), i;
        for (i = 0; i < len; ++i)
            if (((*this)[i] != ' ') && ((*this)[i] != '\t')) break;
        int j = i;
        for (; j < len; ++j)
            if (((*this)[j] == ' ') || ((*this)[j] == '\t')) break;
        str = substr(i, j - i);
        try
        {
            l = stol(str);
        }
        catch (...)
        {
            fprintf(stderr, "<%s:%d> Error stol(%s)\n", __func__, __LINE__, str.c_str());
        }
        *this = substr(j);
        return *this;
    }
};

#endif
