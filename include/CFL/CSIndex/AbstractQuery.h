//
// Created by ShiQingkai on 2021/7/24.
//

#ifndef CS_INDEXING_ABSTRACTQUERY_H
#define CS_INDEXING_ABSTRACTQUERY_H

class AbstractQuery {
public:
    virtual bool reach(int src, int dst) = 0;

    virtual const char *method() const = 0;

    virtual void reset() = 0;
};

#endif //CS_INDEXING_ABSTRACTQUERY_H
