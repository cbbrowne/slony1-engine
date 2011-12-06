-- This DDL will work fine, on one node, or many...
create table t_sample (id serial primary key, content text not null unique);

insert into t_sample (content)
values ('a'),('b'),('c'),('d');

drop table t_sample;
